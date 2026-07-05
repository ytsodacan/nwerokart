#ifndef CLOUDFLARE_TUNNEL_H
#define CLOUDFLARE_TUNNEL_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace Net {

// Wraps `cloudflared tunnel --url http://localhost:PORT` as a child process.
// Cloudflare's free Quick Tunnel proxies HTTP/WebSocket traffic to a random
// `https://xxxxx.trycloudflare.com` hostname with no account or DNS setup needed,
// which is what lets a guest just paste a URL and connect - no port forwarding,
// no cloudflared install on their end.
//
// Requires `cloudflared` to already be installed and on PATH.
class CloudflareTunnel {
  public:
    ~CloudflareTunnel();

    // Starts cloudflared pointed at the given local port. Non-blocking; poll
    // IsReady()/GetPublicUrl() (or GetError()) after calling this.
    bool Start(uint16_t localPort);
    void Stop();

    bool IsRunning() const {
        return mRunning.load();
    }
    bool IsReady() const {
        return mReady.load();
    }
    std::string GetPublicUrl() const;
    // wss:// form of GetPublicUrl(), ready to hand to NetSession::Connect().
    std::string GetPublicWsUrl() const;
    std::string GetError() const;

  private:
    void ReadLoop();

    std::atomic<bool> mRunning{ false };
    std::atomic<bool> mReady{ false };
    mutable std::mutex mMutex;
    std::string mPublicUrl;
    std::string mError;
    std::thread mThread;

#if defined(_WIN32)
    void* mProcessHandle = nullptr;
#else
    int mChildPid = -1;
#endif
    FILE* mPipe = nullptr;
};

} // namespace Net

#endif // CLOUDFLARE_TUNNEL_H
