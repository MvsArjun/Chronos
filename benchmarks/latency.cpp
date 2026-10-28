#include <chronos/concurrency/spsc_ring.hpp>
#include <chronos/core/types.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

struct Clock {
    using time_point = std::int64_t;
    static std::int64_t frequency() {
#ifdef _WIN32
        LARGE_INTEGER value; QueryPerformanceFrequency(&value); return value.QuadPart;
#else
        return 1'000'000'000;
#endif
    }
    static time_point now() {
#ifdef _WIN32
        LARGE_INTEGER value; QueryPerformanceCounter(&value); return value.QuadPart;
#else
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
    }
};
static double nanos(std::int64_t d){return static_cast<double>(d)*(1e9/static_cast<double>(Clock::frequency()));}
static void distribution(const char* name,std::vector<double> values) {
    std::sort(values.begin(),values.end());
    const auto q=[&](double p){return values[static_cast<std::size_t>(p*static_cast<double>(values.size()-1))];};
    std::cout<<'"'<<name<<"\":{\"count\":"<<values.size()<<",\"p50_ns\":"<<q(.5)<<",\"p90_ns\":"<<q(.9)<<",\"p99_ns\":"<<q(.99)<<",\"p999_ns\":"<<q(.999)<<'}';
}
int main() {
    constexpr std::size_t count=100'000,warmup=10'000;
    std::vector<double> clock_cost(count),roundtrip(count),transit(count);
    chronos::concurrency::SpscRing<std::uint64_t,1024> q;
    for(std::size_t i=0;i<count+warmup;++i) {
        auto start=Clock::now();auto finish=Clock::now();if(i>=warmup)clock_cost[i-warmup]=nanos(finish-start);
        start=Clock::now();q.try_push(std::uint64_t{i});auto value=q.try_pop();finish=Clock::now();
        if(!value || *value!=i) return 1;
        if(i>=warmup)roundtrip[i-warmup]=nanos(finish-start);
    }
    struct Timed {Clock::time_point sent;std::size_t sequence;};
    chronos::concurrency::SpscRing<Timed,1024> cross;
    std::jthread producer([&]{for(std::size_t i=0;i<count+warmup;++i)while(!cross.try_push(Timed{Clock::now(),i})) std::this_thread::yield();});
    for(std::size_t i=0;i<count+warmup;++i) {
        std::optional<Timed> item;while(!(item=cross.try_pop()))std::this_thread::yield();
        const auto end=Clock::now();if(item->sequence!=i)return 1;
        if(i>=warmup)transit[i-warmup]=nanos(end-item->sent);
    }
    producer.join();
    std::cout<<"{\"timer_frequency_hz\":"<<Clock::frequency()<<",\"timer_tick_ns\":"<<nanos(1)<<",\"event_size\":"<<sizeof(chronos::Event)<<",\"event_alignment\":"<<alignof(chronos::Event)<<",\"header_size\":"<<sizeof(chronos::EventHeader)<<",\"atomic_size_t_lock_free\":"<<(std::atomic<std::size_t>::is_always_lock_free?"true":"false")<<',';
    distribution("clock_pair",std::move(clock_cost));std::cout<<',';
    distribution("spsc_pair",std::move(roundtrip));std::cout<<',';
    distribution("queue_residence",std::move(transit));std::cout<<"}\n";
}
