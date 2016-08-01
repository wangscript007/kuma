/* Copyright (c) 2014, Fengping Bao <jamol@live.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef __EventLoopImpl_H__
#define __EventLoopImpl_H__

#include "evdefs.h"
#include "util/kmqueue.h"
#include "TimerManager.h"

#ifdef KUMA_OS_WIN
# include <Ws2tcpip.h>
#endif
#include <stdint.h>
#include <thread>
#include <list>

KUMA_NS_BEGIN

class IOPoll;

class EventLoopImpl
{
public:
    class Listener
    {
    public:
        virtual ~Listener() {}
        virtual void loopStopped() = 0;
    };
    
public:
    EventLoopImpl(PollType poll_type = POLL_TYPE_NONE);
    ~EventLoopImpl();

public:
    bool init();
    int registerFd(SOCKET_FD fd, uint32_t events, IOCallback cb);
    int updateFd(SOCKET_FD fd, uint32_t events);
    int unregisterFd(SOCKET_FD fd, bool close_fd);
    TimerManagerPtr getTimerMgr() { return timer_mgr_; }
    
    void addListener(Listener *l);
    void removeListener(Listener *l);
    
    PollType getPollType() const;
    bool isPollLT() const; // level trigger
    
public:
    bool isInEventLoopThread() const { return std::this_thread::get_id() == thread_id_; }
    int runInEventLoop(LoopCallback cb);
    int runInEventLoopSync(LoopCallback cb);
    int queueInEventLoop(LoopCallback cb);
    void loopOnce(uint32_t max_wait_ms);
    void loop(uint32_t max_wait_ms = -1);
    void notify();
    void stop();
    
private:
    typedef std::recursive_mutex KM_Mutex;
    typedef std::lock_guard<KM_Mutex> KM_Lock_Guard;
    
    typedef KM_QueueMT<LoopCallback, KM_Mutex> CallbackQueue;
    
    IOPoll*         poll_;
    bool            stop_loop_;
    std::thread::id thread_id_;
    
    CallbackQueue   cb_queue_;
    
    TimerManagerPtr timer_mgr_;
    
    std::list<Listener*> listeners_;
};

KUMA_NS_END

#endif
