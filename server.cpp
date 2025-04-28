#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

constexpr uint16_t PORT = 8080;
enum Cmd : uint8_t { UPLOAD = 0x01, START = 0x02, STATUS = 0x03 };

struct Job {
    int n = 0;
    int threads = 1;
    std::vector<int32_t> matrix;
    std::vector<int32_t> result;
    std::atomic<int> state{0}; // 0 = idle, 1 = running, 2 = done
    std::mutex mtx;
};

bool sendAll(int s,const void* b,size_t n){const char* p=(const char*)b;while(n){ssize_t k=send(s,p,n,0);if(k<=0)return false;p+=k;n-=k;}return true;}
bool recvAll(int s,void* b,size_t n){char* p=(char*)b;while(n){ssize_t k=recv(s,p,n,0);if(k<=0)return false;p+=k;n-=k;}return true;}

void compute(Job& job) {
    std::lock_guard<std::mutex> lock(job.mtx);
    job.state = 1;
    std::cout << "[*] Starting computation on " << job.threads << " thread(s)...\n";

    job.result = job.matrix;
    auto worker = [&](int tid) {
        for (int i = tid * 2; i + 1 < job.n; i += job.threads * 2) {
            std::cout << "[DEBUG][Thread " << tid << "] Swapping row " << i << " with " << (i + 1) << "\n";
            for (int j = 0; j < job.n; ++j) {
                std::swap(job.result[i * job.n + j], job.result[(i + 1) * job.n + j]);
            }
        }
    };

    std::vector<std::thread> ts;
    for (int i = 0; i < job.threads; ++i)
        ts.emplace_back(worker, i);
    for (auto& t : ts) t.join();

    std::cout << "[*] Computation finished.\n";
    job.state = 2;
}

int main() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(PORT); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    if (listen(srv, 10) < 0) {
        perror("listen"); return 1;
    }

    std::cout << "[*] Server listening on port " << PORT << "...\n";

    Job sharedJob; // Один спільний job для всіх клієнтів, можна адаптувати

    auto handleClient = [&](int cli) {
        std::cout << "[*] New client thread started.\n";

        while (true) {
            uint8_t cmd; uint32_t lenNet;
            if (!recvAll(cli, &cmd, 1) || !recvAll(cli, &lenNet, 4)) break;
            uint32_t len = ntohl(lenNet);

            std::cout << "[DEBUG] Command received: 0x" << std::hex << int(cmd) << std::dec << ", payload size = " << len << "\n";

            switch (cmd) {
                case UPLOAD: {
                    if (len < 8) { std::cerr << "[!] Invalid payload length.\n"; break; }

                    uint32_t nNet, thrNet;
                    recvAll(cli, &nNet, 4); recvAll(cli, &thrNet, 4);
                    sharedJob.n = ntohl(nNet); sharedJob.threads = ntohl(thrNet);
                    sharedJob.matrix.resize(sharedJob.n * sharedJob.n);

                    if (!recvAll(cli, sharedJob.matrix.data(), sharedJob.n * sharedJob.n * 4)) {
                        std::cerr << "[!] Failed to receive matrix.\n"; break;
                    }

                    std::cout << "[DEBUG] Matrix uploaded: size " << sharedJob.n << "x" << sharedJob.n << ", threads = " << sharedJob.threads << "\n";

                    sharedJob.state = 0;
                    uint8_t ack = 0x00; uint32_t l = htonl(0);
                    sendAll(cli, &ack, 1); sendAll(cli, &l, 4);
                    break;
                }

                case START: {
                    std::cout << "[DEBUG] START command received\n";
                    std::thread(compute, std::ref(sharedJob)).detach();
                    uint8_t ack = 0x00; uint32_t l = htonl(0);
                    sendAll(cli, &ack, 1); sendAll(cli, &l, 4);
                    break;
                }

                case STATUS: {
                    std::cout << "[DEBUG] STATUS command received\n";
                    uint8_t ack = 0x00;
                    uint8_t st = sharedJob.state.load();
                    std::cout << "[DEBUG] Current job state = " << (int)st << "\n";

                    if (st == 2) {
                        uint32_t payloadLen = 1 + sharedJob.n * sharedJob.n * 4;
                        uint32_t l = htonl(payloadLen);
                        sendAll(cli, &ack, 1); sendAll(cli, &l, 4);
                        sendAll(cli, &st, 1);
                        sendAll(cli, sharedJob.result.data(), sharedJob.n * sharedJob.n * 4);
                        std::cout << "[*] Result sent to client.\n";
                    } else {
                        uint32_t l = htonl(1);
                        sendAll(cli, &ack, 1); sendAll(cli, &l, 4);
                        sendAll(cli, &st, 1);
                    }
                    break;
                }

                default:
                    std::cerr << "[!] Unknown command received: 0x" << std::hex << int(cmd) << std::dec << '\n';
            }
        }

        std::cout << "[*] Client disconnected.\n";
        close(cli);
    };

    while (true) {
        int cli = accept(srv, nullptr, nullptr);
        if (cli < 0) { perror("accept"); continue; }

        std::cout << "[*] Client connected!\n";
        std::thread(handleClient, cli).detach();
    }

    close(srv);
}

