#include "peripheral/crypto/sha256.hpp"
#include "peripheral/storage/cloud_uploader.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace peripheral;

void test_sha256() {
    std::cout << "[Test 1] Testing streaming SHA-256 implementation...\n";

    // 1. Known test vector: empty string
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    std::string hash_empty = Sha256::hash_string("");
    assert(hash_empty == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    // 2. Known test vector: "hello"
    // 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    std::string hash_hello = Sha256::hash_string("hello");
    assert(hash_hello == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

    // 3. Known test vector: "The quick brown fox jumps over the lazy dog"
    // d7a8fbb307d7809469ca9ab00020e301a571f50b736a04146e5fe5c8a057a37d
    std::string hash_fox = Sha256::hash_string("The quick brown fox jumps over the lazy dog");
    assert(hash_fox == "d7a8fbb307d7809469ca9ab00020e301a571f50b736a04146e5fe5c8a057a37d");

    // 4. File hashing test
    std::filesystem::path test_file = "test_sha_tmp.bin";
    {
        std::ofstream out(test_file, std::ios::binary);
        out << "hello";
    }
    std::string hash_file = Sha256::hash_file(test_file);
    assert(hash_file == hash_hello);
    std::filesystem::remove(test_file);

    std::cout << "  -> SHA-256 test vectors PASSED!\n";
}

void test_queue_and_telemetry() {
    std::cout << "[Test 2] Testing CloudUploader queue and telemetry...\n";

    std::filesystem::path test_dir = "test_upload_rec";
    std::filesystem::create_directories(test_dir / "rec-001");
    std::filesystem::path chunk1 = test_dir / "rec-001" / "part-0001.bin";
    {
        std::ofstream out(chunk1, std::ios::binary);
        std::string payload(1024, 'A');
        out << payload;
    }

    CloudUploaderConfig cfg;
    cfg.record_dir = test_dir;
    cfg.port = 65530; // Not running, just test queue mechanics
    CloudUploader uploader(cfg);

    auto t0 = uploader.get_telemetry();
    assert(t0.backlog_objects == 0);
    assert(t0.backlog_bytes == 0);
    assert(t0.storage == "disconnected");

    uploader.enqueue("rec-001", "rec-001/part-0001.bin", chunk1, false);

    auto t1 = uploader.get_telemetry();
    assert(t1.backlog_objects == 1);
    assert(t1.backlog_bytes == 1024);
    assert(t1.oldest_pending_us.has_value());

    // Deduplication check: enqueue same object again
    uploader.enqueue("rec-001", "rec-001/part-0001.bin", chunk1, false);
    auto t2 = uploader.get_telemetry();
    assert(t2.backlog_objects == 1);

    std::filesystem::remove_all(test_dir);
    std::cout << "  -> Queue & Telemetry test PASSED!\n";
}

void test_mock_storage_http_put() {
    std::cout << "[Test 3] Testing HTTP PUT against mock cloud storage server...\n";

    // 1. Create a local mock HTTP server on an ephemeral port
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server_fd >= 0);

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // Bind to ephemeral port
    assert(bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(listen(server_fd, 1) == 0);

    socklen_t addr_len = sizeof(addr);
    assert(getsockname(server_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0);
    (void)addr_len;
    uint16_t port = ntohs(addr.sin_port);

    // 2. Background thread to handle the mock HTTP request
    std::thread server_thread([server_fd] {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) return;

        char buf[4096];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::string req(buf);
            assert(req.find("PUT /v1/servers/peripheral-1/objects/rec-001/part-0001.bin HTTP/1.1") != std::string::npos);
            assert(req.find("Authorization: Bearer dev-token") != std::string::npos);
            assert(req.find("X-Content-SHA256: ") != std::string::npos);

            // Send 201 Created response
            std::string resp = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 20\r\n\r\n{\"status\":\"created\"}";
            send(client_fd, resp.data(), resp.size(), 0);
        }
        close(client_fd);
        close(server_fd);
    });

    // 3. Test upload through CloudUploader
    std::filesystem::path test_dir = "test_upload_mock";
    std::filesystem::create_directories(test_dir / "rec-001");
    std::filesystem::path chunk_file = test_dir / "rec-001" / "part-0001.bin";
    {
        std::ofstream out(chunk_file, std::ios::binary);
        out << "chunk test data 12345";
    }

    CloudUploaderConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.server_id = "peripheral-1";
    cfg.auth_token = "dev-token";
    cfg.record_dir = test_dir;

    CloudUploader uploader(cfg);
    UploadTask task{
        .recording_id = "rec-001",
        .object_key = "rec-001/part-0001.bin",
        .file_path = chunk_file,
        .file_size = 21,
        .created_at_us = 100,
        .attempts = 0,
        .is_manifest = false
    };

    bool ok = uploader.upload_file_sync(task);
    assert(ok);
    (void)ok;

    server_thread.join();
    std::filesystem::remove_all(test_dir);
    std::cout << "  -> Mock Cloud Storage HTTP PUT test PASSED!\n";
}

int main() {
    std::cout << "=========================================\n";
    std::cout << " Running CloudUploader & SHA-256 Tests   \n";
    std::cout << "=========================================\n";

    test_sha256();
    test_queue_and_telemetry();
    test_mock_storage_http_put();

    std::cout << "\nALL CLOUD UPLOADER TESTS PASSED!\n";
    return 0;
}
