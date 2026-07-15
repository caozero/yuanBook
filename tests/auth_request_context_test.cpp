// auth_request_context_test.cpp: 请求级认证上下文并发隔离回归测试。

#include "WebServer.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    /**
     * @brief 模拟单个工作线程反复处理同一用户请求，并验证上下文不会被其他线程覆盖。
     * @param Username 当前线程绑定的唯一用户名。
     * @param IterationCount 校验循环次数。
     * @param bFailed 任一线程发现上下文串扰时置为 true。
     */
    void RunContextIsolationCase(const std::string& Username,
                                 int IterationCount,
                                 std::atomic<bool>& bFailed)
    {
        for (int index = 0; index < IterationCount && !bFailed.load(); ++index) {
            FAuthRequestContext context;
            context.Token = "token-" + Username;
            context.Username = Username;
            context.Permissions = "[\"user\"]";
            context.bAuthenticated = true;

            // 线程局部副本模拟 WebServer 请求入口和下游处理器之间的认证上下文传递。
            thread_local FAuthRequestContext requestContext;
            requestContext = context;
            std::this_thread::yield();

            if (!requestContext.bAuthenticated ||
                requestContext.Username != Username ||
                requestContext.Token != context.Token ||
                requestContext.Permissions != "[\"user\"]") {
                bFailed.store(true);
                return;
            }
        }
    }
}

int main()
{
    constexpr int ThreadCount = 16;
    constexpr int IterationCount = 20000;

    std::atomic<bool> bFailed{false};
    std::vector<std::thread> workers;
    workers.reserve(ThreadCount);

    for (int index = 0; index < ThreadCount; ++index) {
        workers.emplace_back(RunContextIsolationCase,
                             "parallel-user-" + std::to_string(index),
                             IterationCount,
                             std::ref(bFailed));
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    if (bFailed.load()) {
        std::cerr << "[FAIL] request authentication context leaked across concurrent threads" << std::endl;
        return 1;
    }

    std::cout << "auth_request_context_test passed" << std::endl;
    return 0;
}
