#include <cassert>
#include <set>
#include <vector>
#include <map>
#include <thread>
#include <string>
#include <cstring>

#include <brynet/net/SocketLibFunction.h>
#include <brynet/net/fdset.h>

#include <brynet/net/Connector.h>

using namespace brynet;
using namespace brynet::net;

namespace brynet
{
    namespace net
    {
        class AsyncConnectAddr
        {
        public:
            AsyncConnectAddr(const std::string& ip, 
                int port, 
                std::chrono::nanoseconds timeout, 
                const AsyncConnector::COMPLETED_CALLBACK& successCB,
                const AsyncConnector::FAILED_CALLBACK& failedCB) : 
                mIP(ip),
                mPort(port), 
                mTimeout(timeout), 
                mSuccessCB(successCB),
                mFailedCB(failedCB)
            {
            }

            const std::string&         getIP() const
            {
                return mIP;
            }

            int                         getPort() const
            {
                return mPort;
            }

            const AsyncConnector::COMPLETED_CALLBACK&   getSuccessCB() const
            {
                return mSuccessCB;
            }

            const AsyncConnector::FAILED_CALLBACK&  getFailedCB() const
            {
                return mFailedCB;
            }

            std::chrono::nanoseconds                getTimeout() const
            {
                return mTimeout;
            }

        private:
            std::string                         mIP;
            int                                 mPort;
            std::chrono::nanoseconds            mTimeout;
            AsyncConnector::COMPLETED_CALLBACK  mSuccessCB;
            AsyncConnector::FAILED_CALLBACK     mFailedCB;
        };

        class ConnectorWorkInfo final : public NonCopyable
        {
        public:
            typedef std::shared_ptr<ConnectorWorkInfo>    PTR;

            ConnectorWorkInfo() BRYNET_NOEXCEPT;

            void                checkConnectStatus(int millsecond);
            bool                isConnectSuccess(sock clientfd) const;
            void                checkTimeout();
            void                processConnect(const AsyncConnectAddr&);
            void                causeAllFailed();

        private:

            struct ConnectingInfo
            {
                ConnectingInfo()
                {
                    timeout = std::chrono::nanoseconds::zero();
                }

                std::chrono::steady_clock::time_point   startConnectTime;
                std::chrono::nanoseconds                timeout;
                AsyncConnector::COMPLETED_CALLBACK      successCB;
                AsyncConnector::FAILED_CALLBACK         failedCB;
            };

            std::map<sock, ConnectingInfo>  mConnectingInfos;
            std::set<sock>                  mConnectingFds;

            struct FDSetDeleter
            {
                void operator()(struct fdset_s* ptr) const
                {
                    ox_fdset_delete(ptr);
                }
            };

            std::unique_ptr<struct fdset_s, FDSetDeleter> mFDSet;
        };
    }
}

ConnectorWorkInfo::ConnectorWorkInfo() BRYNET_NOEXCEPT
{
    mFDSet.reset(ox_fdset_new());
}

bool ConnectorWorkInfo::isConnectSuccess(sock clientfd) const
{
    if (!ox_fdset_check(mFDSet.get(), clientfd, WriteCheck))
    {
        return false;
    }

    int error;
    int len = sizeof(error);
    if (getsockopt(clientfd, SOL_SOCKET, SO_ERROR, (char*)&error, (socklen_t*)&len) == -1)
    {
        return false;
    }

    return error == 0;
}

void ConnectorWorkInfo::checkConnectStatus(int millsecond)
{
    if (ox_fdset_poll(mFDSet.get(), millsecond) <= 0)
    {
        return;
    }

    std::set<sock>  total_fds;
    std::set<sock>  success_fds;

    for (auto& v : mConnectingFds)
    {
        if (ox_fdset_check(mFDSet.get(), v, WriteCheck))
        {
            total_fds.insert(v);
            if (isConnectSuccess(v))
            {
                success_fds.insert(v);
            }
        }
    }

    for (auto fd : total_fds)
    {
        ox_fdset_del(mFDSet.get(), fd, WriteCheck);
        mConnectingFds.erase(fd);

        auto it = mConnectingInfos.find(fd);
        if (it == mConnectingInfos.end())
        {
            continue;
        }

        auto socket = TcpSocket::Create(fd, false);
        if (success_fds.find(fd) != success_fds.end())
        {
            if (it->second.successCB != nullptr)
            {
                it->second.successCB(std::move(socket));
            }
        }
        else
        {
            if (it->second.failedCB != nullptr)
            {
                it->second.failedCB();
            }
        }
        
        mConnectingInfos.erase(it);
    }
}

void ConnectorWorkInfo::checkTimeout()
{
    for (auto it = mConnectingInfos.begin(); it != mConnectingInfos.end();)
    {
        auto now = std::chrono::steady_clock::now();
        if ((now - it->second.startConnectTime) < it->second.timeout)
        {
            ++it;
            continue;
        }
        
        auto fd = it->first;
        auto cb = it->second.failedCB;

        ox_fdset_del(mFDSet.get(), fd, WriteCheck);

        mConnectingFds.erase(fd);
        mConnectingInfos.erase(it++);

        brynet::net::base::SocketClose(fd);
        if (cb != nullptr)
        {
            cb();
        }
    }
}

void ConnectorWorkInfo::causeAllFailed()
{
    for (auto it = mConnectingInfos.begin(); it != mConnectingInfos.end();)
    {
        auto fd = it->first;
        auto cb = it->second.failedCB;

        ox_fdset_del(mFDSet.get(), fd, WriteCheck);

        mConnectingFds.erase(fd);
        mConnectingInfos.erase(it++);

        brynet::net::base::SocketClose(fd);
        if (cb != nullptr)
        {
            cb();
        }
    }
}

void ConnectorWorkInfo::processConnect(const AsyncConnectAddr& addr)
{
    struct sockaddr_in server_addr;
    sock clientfd = SOCKET_ERROR;
    ConnectingInfo ci;

#if defined PLATFORM_WINDOWS
    int check_error = WSAEWOULDBLOCK;
#else
    int check_error = EINPROGRESS;
#endif
    int n = 0;

    brynet::net::base::InitSocket();

    clientfd = brynet::net::base::SocketCreate(AF_INET, SOCK_STREAM, 0);
    if (clientfd == SOCKET_ERROR)
    {
        goto FAILED;
    }

    brynet::net::base::SocketNonblock(clientfd);
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, addr.getIP().c_str(), &server_addr.sin_addr.s_addr);
    server_addr.sin_port = htons(addr.getPort());

    n = connect(clientfd, (struct sockaddr*)&server_addr, sizeof(struct sockaddr));
    if (n == 0)
    {
        goto SUCCESS;
    }

    if (check_error != sErrno)
    {
        brynet::net::base::SocketClose(clientfd);
        clientfd = SOCKET_ERROR;
        goto FAILED;
    }

    ci.startConnectTime = std::chrono::steady_clock::now();
    ci.successCB = addr.getSuccessCB();
    ci.failedCB = addr.getFailedCB();
    ci.timeout = addr.getTimeout();

    mConnectingInfos[clientfd] = ci;
    mConnectingFds.insert(clientfd);
    ox_fdset_add(mFDSet.get(), clientfd, WriteCheck);
    return;

SUCCESS:
    if (addr.getSuccessCB() != nullptr)
    {
        addr.getSuccessCB()(TcpSocket::Create(clientfd, false));
    }
    return;

FAILED:
    if (addr.getFailedCB() != nullptr)
    {
        addr.getFailedCB()();
    }
}

AsyncConnector::AsyncConnector()
{
    mIsRun = std::make_shared<bool>(false);
}

AsyncConnector::~AsyncConnector()
{
    stopWorkerThread();
}

static void runOnceCheckConnect(const std::shared_ptr<brynet::net::EventLoop>& eventLoop,
    const std::shared_ptr<ConnectorWorkInfo>& workerInfo)
{
    eventLoop->loop(std::chrono::milliseconds(10).count());
    workerInfo->checkConnectStatus(0);
    workerInfo->checkTimeout();
}

void AsyncConnector::startWorkerThread()
{
#ifdef HAVE_LANG_CXX17
    std::lock_guard<std::shared_mutex> lck(mThreadGuard);
#else
    std::lock_guard<std::mutex> lck(mThreadGuard);
#endif

    if (mThread != nullptr)
    {
        return;
    }

    mIsRun = std::make_shared<bool>(true);
    mWorkInfo = std::make_shared<ConnectorWorkInfo>();
    mEventLoop = std::make_shared<EventLoop>();

    auto eventLoop = mEventLoop;
    auto workerInfo = mWorkInfo;
    auto isRun = mIsRun;

    mThread = std::make_shared<std::thread>([eventLoop, workerInfo, isRun](){
        while (*isRun)
        {
            runOnceCheckConnect(eventLoop, workerInfo);
        }

        workerInfo->causeAllFailed();
    });
}

void AsyncConnector::stopWorkerThread()
{
#ifdef HAVE_LANG_CXX17
    std::lock_guard<std::shared_mutex> lck(mThreadGuard);
#else
    std::lock_guard<std::mutex> lck(mThreadGuard);
#endif

    if (mThread == nullptr)
    {
        return;
    }

    mEventLoop->pushAsyncProc([this]() {
        *mIsRun = false;
    });

    try
    {
        if (mThread->joinable())
        {
            mThread->join();
        }
    }
    catch(...)
    { }

    mEventLoop = nullptr;
    mWorkInfo = nullptr;
    mIsRun = nullptr;
    mThread = nullptr;
}

void AsyncConnector::asyncConnect(const std::string& ip, 
    int port, 
    std::chrono::nanoseconds timeout,
    COMPLETED_CALLBACK successCB, 
    FAILED_CALLBACK failedCB)
{
#ifdef HAVE_LANG_CXX17
    std::shared_lock<std::shared_mutex> lck(mThreadGuard);
#else
    std::lock_guard<std::mutex> lck(mThreadGuard);
#endif

    if (successCB == nullptr || failedCB == nullptr)
    {
        throw std::runtime_error("all callback is nullptr");
    }

    if (!(*mIsRun))
    {
        throw std::runtime_error("work thread already stop");
    }

    auto workInfo = mWorkInfo;
    auto address = AsyncConnectAddr(ip,
        port,
        timeout,
        successCB,
        failedCB);
    mEventLoop->pushAsyncProc([workInfo, address]() {
        workInfo->processConnect(address);
    });
}

AsyncConnector::PTR AsyncConnector::Create()
{
    struct make_shared_enabler : public AsyncConnector {};
    return std::make_shared<make_shared_enabler>();
}
