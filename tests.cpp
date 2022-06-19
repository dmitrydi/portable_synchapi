#include "tests.h"
#include "test_runner.h"
#include "port_synchro.h"
#include <chrono>
#include <thread>
#include <vector>

void RunTests() {
    TestRunner tr;
    RUN_TEST(tr, TestWeakMutex);
    //RUN_TEST(tr, TestWaitForSingleObject_1);
    RUN_TEST(tr, TestWaitForSingleObject_2);
}

void TestWeakMutex() {
    WeakMutex wmtx;
    auto locker = [&](){
        wmtx.weak_lock();
    };
    auto unlocker = [&](int sleeping_time){
        std::this_thread::sleep_for(std::chrono::milliseconds(sleeping_time));
        wmtx.unlock();
    };
    auto tester = [&](int& measured_duration){
        auto start = std::chrono::system_clock::now();
        wmtx.hard_lock();
        measured_duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start).count();
    };
    int nlockers = 5;
    std::vector<std::thread> locker_threads;
    for (int i = 0; i != nlockers; ++i)
        locker_threads.emplace_back(std::thread(locker));
    for (int i = 0; i != nlockers; ++i)
        locker_threads[i].join();
    int elapsed;
    int sleeping_time = 1000;
    std::thread worker(tester, std::ref(elapsed));
    std::thread unlk(unlocker, sleeping_time);
    unlk.join();
    worker.join();
    ASSERT(elapsed >= sleeping_time);
}

void TestWaitForSingleObject_1() {
    Event ev(false, false);
    int duration = 2000;
    auto wt = [](Event* event, int dur, int& res) {
        res = WaitForSingleObject(event, dur);
    };
    int num_waiters = 1000;
    std::vector<std::thread> waiters;
    std::vector<int> results(num_waiters, 100500);
    for (int i = 0; i != num_waiters; ++i) {
        waiters.emplace_back(std::thread(wt, &ev, duration, std::ref(results[i])));
        if (i % 30 == 0)
            ev.set();
    }
    for (int i = 0; i != num_waiters; ++i)
        waiters[i].join();
    int signaled_count = 0;
    for (int i = 0; i != num_waiters; ++i) {
        if (!results[i])
            ++signaled_count;
    }
    std::cout << "Number of waiters OK: " << signaled_count << std::endl;
    std::cout << "Number of waiters for event: " << ev.get_waiters_num() << std::endl;
}

void TestWaitForSingleObject_2() {
    portable::Event ev(false, false);
    int duration = 2000;
    auto wt = [](portable::Event* event, int dur, int& res) {
        res = portable::WaitForSingleObject(event, dur);
    };
    int num_waiters = 1000;
    std::vector<std::thread> waiters;
    std::vector<int> results(num_waiters, 100500);
    for (int i = 0; i != num_waiters; ++i) {
        waiters.emplace_back(std::thread(wt, &ev, duration, std::ref(results[i])));
        if (i % 30 == 0)
            ev.set();
    }
    for (int i = 0; i != num_waiters; ++i)
        waiters[i].join();
    int signaled_count = 0;
    for (int i = 0; i != num_waiters; ++i) {
        if (!results[i])
            ++signaled_count;
    }
    std::cout << "Number of waiters OK: " << signaled_count << std::endl;
    //std::cout << "Number of waiters for event: " << ev.get_waiters_num() << std::endl;
}
