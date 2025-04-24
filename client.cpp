#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <random>



constexpr uint16_t PORT = 8080;
enum Cmd : uint8_t { UPLOAD = 0x01, START = 0x02, STATUS = 0x03 };

bool sendAll(int s,const void* b,size_t n){const char* p=(const char*)b;while(n){ssize_t k=send(s,p,n,0);if(k<=0)return false;p+=k;n-=k;}return true;}
bool recvAll(int s,void* b,size_t n){char* p=(char*)b;while(n){ssize_t k=recv(s,p,n,0);if(k<=0)return false;p+=k;n-=k;}return true;}

void printMatrix(const std::vector<int32_t>& m, int n, const std::string& label) {
    std::cout << label << ":\n";
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) std::cout << m[i*n + j] << ' ';
        std::cout << '\n';
    }
}

int main() {
    const int n = 6;
    const int threads = 4;

    std::vector<int32_t> m(n*n);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 99);
    for (auto& x : m) x = dist(gen);

    printMatrix(m, n, "[*] Original matrix");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock<0){perror("socket");return 1;}

    sockaddr_in srv{}; srv.sin_family=AF_INET; srv.sin_port=htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&srv.sin_addr);
    if (connect(sock,reinterpret_cast<sockaddr*>(&srv),sizeof(srv))) {
        perror("connect"); return 1;
    }

    std::cout << "[*] Connected to server\n";

    /* ---- UPLOAD ---- */
    uint8_t cmd = UPLOAD; uint32_t len = htonl(8 + n*n*4);
    sendAll(sock,&cmd,1); sendAll(sock,&len,4);

    uint32_t nNet = htonl(n), thrNet = htonl(threads);
    sendAll(sock,&nNet,4); sendAll(sock,&thrNet,4);
    sendAll(sock,m.data(), n*n*4);

    std::cout << "[*] Sent matrix (" << n << "x" << n << "), threads=" << threads << '\n';

    /* ack */
    uint8_t ack; uint32_t l; recvAll(sock,&ack,1); recvAll(sock,&l,4);
    std::cout << "[*] Received upload ACK\n";

    /* ---- START ---- */
    cmd = START; len = 0;
    sendAll(sock,&cmd,1); sendAll(sock,&len,4);
    recvAll(sock,&ack,1); recvAll(sock,&l,4);
    std::cout << "[*] Sent START command\n";

    /* ---- poll STATUS until DONE ---- */
    uint8_t state = 0;
    std::vector<int32_t> res;
    while (true) {
        cmd = STATUS; len = 0;
        sendAll(sock,&cmd,1); sendAll(sock,&len,4);

        recvAll(sock,&ack,1); recvAll(sock,&l,4);
        uint32_t payload = ntohl(l);
        if (!recvAll(sock,&state,1)) { std::cerr<<"[!] Network error\n"; return 1; }

        if (state == 2) {                        // DONE
            std::cout << "[*] Job DONE. Receiving result...\n";
            res.resize(n*n);
            recvAll(sock,res.data(), n*n*4);
            break;
        }
        else {
            std::cout << "[*] Status: still running (" << (int)state << ")...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    printMatrix(res, n, "[*] Result matrix");
    close(sock);
}