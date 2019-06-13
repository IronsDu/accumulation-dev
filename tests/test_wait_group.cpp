#define CATCH_CONFIG_MAIN  // This tells Catch to provide a main() - only do this in one cpp file
#include "catch.hpp"
#include <memory>
#include <atomic>
#include <brynet/utils/WaitGroup.h>
#include <chrono>
#include <thread>

TEST_CASE("WaitGroup are computed", "[waitgroup]") {
    auto wg = brynet::utils::WaitGroup::Create();
    wg->wait();

    wg->add(2);

    std::atomic<int> upvalue = ATOMIC_VAR_INIT(1);
    auto a = std::thread([&]() {
        upvalue++;
        wg->done();
    });
    auto b = std::thread([&]() {
        upvalue++;
        wg->done();
    });
    wg->wait();

    //REQUIRE(upvalue == 2);
    wg->wait();
    if (a.joinable())
    {
        a.join();
    }
    if (b.joinable())
    {
        b.join();
    }
}