#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <chrono>
#include <iostream>
#include <memory>

class WeakMutex {
private:
    bool locked;
    std::mutex mtx;
    std::condition_variable cv;
public:
    WeakMutex(): locked(false) {}
    void weak_lock() {
        std::lock_guard<std::mutex> lk(mtx);
        locked = true;
    }
    void hard_lock() {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if(!locked) {
                locked = true;
                return;
            }
        }
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [&](){ return !locked; });
        locked = true;
    }
    void unlock() {
        {
           std::lock_guard<std::mutex> lk(mtx);
           locked = false;
        }
        cv.notify_one();
    }

};


struct CommData {
    std::condition_variable& out_cv;
    WeakMutex& out_mtx;
};

class Event {
private:
    const bool is_manual_reset;
    bool is_signaled;
    int num_waiters;
    std::condition_variable cv;
    WeakMutex comm_mtx;
    mutable std::mutex attach_mutex;
    mutable std::mutex count_mutex;

public:
    Event(bool bManualReset, bool bInitialState): is_manual_reset(bManualReset), is_signaled(bInitialState), num_waiters(0) {};
    bool set() {
        std::lock_guard<std::mutex> lk(attach_mutex); // this lock makes impossible the situation when number of waiters
                                                      // increases right after notifying which would cause infinite wait for auto-reset event
        is_signaled = true;
        cv.notify_one();

        if(!is_manual_reset) {
            if(get_waiters_num()) {
                comm_mtx.hard_lock();
                is_signaled = false;
                comm_mtx.unlock();
            } else {
                is_signaled = false;
            }
        }
        return true;
    }
    bool reset() {
        std::lock_guard<std::mutex> lk(attach_mutex);
        is_signaled = false;
        return true;
    }
    bool manual_reset() const {
        return is_manual_reset;
    }
    bool signaled() const {
        std::lock_guard<std::mutex> lk_att(attach_mutex);
        return is_signaled;
    }
    bool not_sync_signaled() const {
        return is_signaled;
    }
    CommData attach_waiter() {
        std::unique_lock<std::mutex> lk(attach_mutex, std::defer_lock);
        std::unique_lock<std::mutex> lk_cnt(count_mutex, std::defer_lock);
        std::lock(lk, lk_cnt);
        ++num_waiters;
        return {cv, comm_mtx};
    }
    int get_waiters_num() const {
        std::lock_guard<std::mutex> lk_cnt(count_mutex);
        return num_waiters;
    }
    int detach_waiter() {
        std::lock_guard<std::mutex> lk_cnt(count_mutex);
        if(num_waiters > 0)
            --num_waiters;
        return num_waiters;
    }
};

constexpr unsigned long WAIT_OK = 0L;
constexpr unsigned long WAIT_TIMEOUT = 258L;

int WaitForSingleObject(Event* ev, unsigned long duration_ms) {
    if (ev->signaled())
        return 0;
    std::mutex mtx;
    auto comms = ev->attach_waiter();
    std::unique_lock<std::mutex> lk(mtx);
    comms.out_mtx.weak_lock();
    auto wait_result = comms.out_cv.wait_for(lk, std::chrono::milliseconds(duration_ms), [&](){ return ev->not_sync_signaled();});
    auto waiters_left = ev->detach_waiter();
    if(!ev->manual_reset()) {
        if (wait_result || !waiters_left)
            comms.out_mtx.unlock();
    }
    return wait_result ? WAIT_OK : WAIT_TIMEOUT;
}


namespace portable {

struct Comms {
    std::condition_variable* cv;
    std::mutex* mtx;
    int* cntr;
    std::vector<bool>* signalsContainer;
    Comms(std::condition_variable* _cv, std::mutex* _mtx, int* _cntr, std::vector<bool>* _sig_cr):
        cv(_cv),
        mtx(_mtx),
        cntr(_cntr),
        signalsContainer(_sig_cr) {}
};

struct CommsWrapper {
    std::weak_ptr<Comms> comms;
    int idxToSignal;
};

class Event {
private:
   const bool bIsManualReset;
   bool bIsSignaled;
   std::mutex appMutex;
   mutable std::mutex stateMutex;
   std::vector<CommsWrapper> waiters;
   std::vector<CommsWrapper> waitersAux;
public:
    Event(bool bManualReset, bool bInitialState): bIsManualReset(bManualReset), bIsSignaled(bInitialState) {}
    bool set() {
        {
            std::lock_guard<std::mutex> st_lk(stateMutex);
            if(bIsManualReset)
                bIsSignaled = true;
            else
                bIsSignaled = false;
        }
        std::lock_guard<std::mutex> lk(appMutex);
        if(!waiters.empty()) {
            waitersAux.reserve(waiters.size());
            for (auto& wt: waiters) {
                auto swt = wt.comms.lock();
                if (!swt)
                    continue;
                std::lock_guard<std::mutex> wlk(*swt->mtx); // here we intentionally hold the waiter since it's not expired in previous line
                ++(*swt->cntr);
                (*swt->signalsContainer)[wt.idxToSignal] = true;
                swt->cv->notify_one();
                break;
            }

            for (auto& w: waiters) {
                if(!w.comms.expired())
                    waitersAux.push_back(w);
            }
            std::swap(waiters, waitersAux);
            waitersAux.clear();

        }
        return true;
    }
    bool reset() {
        std::lock_guard<std::mutex> lk(stateMutex);
        bIsSignaled = false;
        return true;
    }
    bool isSignaled() const {
        std::lock_guard<std::mutex> lk(stateMutex);
        return bIsSignaled;
    }
    bool isManualReset() const {
        return bIsManualReset;
    }
    void attachWaiter(CommsWrapper aWaiter) {
        std::lock_guard<std::mutex> lk(appMutex);
        waiters.push_back(aWaiter);
    }
};

constexpr unsigned long WAIT_OBJECT_0 = 0L;
constexpr unsigned long WAIT_TIMEOUT = 258L;

int WaitForMultipleObjects(std::vector<Event*> vEvents, bool bWaitAll, unsigned long ulMilliseconds) {
    int totalEvents = vEvents.size();
    int signaled = 0;
    for (int i = 0; i != totalEvents; i++) {
        if (vEvents[i] && vEvents[i]->isSignaled()) {
            ++signaled;
        }
        if(!bWaitAll && signaled)
            return WAIT_OBJECT_0 + i;
    }
    if (signaled == totalEvents)
        return WAIT_OBJECT_0;
    std::mutex mtx;
    std::condition_variable cv;
    bool ret = false;
    std::vector<bool> signals(totalEvents, false);
    {
        std::shared_ptr<Comms> comms = std::make_shared<Comms>(&cv, &mtx, &signaled, &signals);
        std::unique_lock<std::mutex> lk(mtx);
        for(int i = 0; i != totalEvents; i++) {
            if(vEvents[i]) {
                vEvents[i]->attachWaiter({comms,i});
            }
        }
        ret = cv.wait_for(
            lk,
            std::chrono::milliseconds(ulMilliseconds),
            [&](){
                    if (bWaitAll)
                        return signaled == totalEvents;
                    else
                        return signaled > 0;
                 }
            );
    }
    if (ret) {
        for(int i = 0; i != totalEvents; ++i) {
            if (signals[i]) {
                return WAIT_OBJECT_0 + i;
            }
        }
    }
    return WAIT_TIMEOUT;
}

int WaitForSingleObject(Event* ev, unsigned long duration_ms) {
    std::vector<Event*> vEv(1, ev);
    return WaitForMultipleObjects(vEv, false, duration_ms);
}


}
