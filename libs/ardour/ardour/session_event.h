#ifndef __ardour_session_event_h__
#define __ardour_session_event_h__

#include <list>
#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>

#include "pbd/pool.h"
#include "pbd/ringbuffer.h"
#include "pbd/event_loop.h"

#include "ardour/types.h"

namespace ARDOUR {

class Slave;
class Region;

struct SessionEvent {
    enum Type {
	    SetTransportSpeed,
	    SetTrackSpeed,
	    Locate,
	    LocateRoll,
	    LocateRollLocate,
	    SetLoop,
	    PunchIn,
	    PunchOut,
	    RangeStop,
	    RangeLocate,
	    Overwrite,
	    SetSyncSource,
	    Audition,
	    InputConfigurationChange,
	    SetPlayAudioRange,
	    RealTimeOperation,
            AdjustPlaybackBuffering,
            AdjustCaptureBuffering,

	    /* only one of each of these events can be queued at any one time */
	    
	    StopOnce,
	    AutoLoop
    };
    
    enum Action {
	    Add,
	    Remove,
	    Replace,
	    Clear
    };
    
    Type             type;
    Action           action;
    nframes64_t      action_frame;
    nframes64_t      target_frame;
    double           speed;
    
    union {
	void*        ptr;
	bool         yes_or_no;
	nframes64_t  target2_frame;
	Slave*       slave;
	Route*       route;
    };
    
    union {
	bool second_yes_or_no;
    };

    /* 4 members to handle a multi-group event handled in RT context */

    typedef boost::function<void (SessionEvent*)> RTeventCallback;

    boost::shared_ptr<RouteList> routes;    /* apply to */
    boost::function<void (void)> rt_slot;   /* what to call in RT context */
    RTeventCallback              rt_return; /* called after rt_slot, with this event as an argument */
    PBD::EventLoop*              event_loop;

    std::list<AudioRange> audio_range;
    std::list<MusicRange> music_range;
    
    boost::shared_ptr<Region> region;

    SessionEvent (Type t, Action a, nframes_t when, nframes_t where, double spd, bool yn = false, bool yn2 = false)
	    : type (t)
	    , action (a)
	    , action_frame (when)
	    , target_frame (where)
	    , speed (spd)
	    , yes_or_no (yn)
	    , second_yes_or_no (yn2)
	    , event_loop (0) {}

    void set_ptr (void* p) {
	    ptr = p;
    }
    
    bool before (const SessionEvent& other) const {
	    return action_frame < other.action_frame;
    }
    
    bool after (const SessionEvent& other) const {
	    return action_frame > other.action_frame;
    }
    
    static bool compare (const SessionEvent *e1, const SessionEvent *e2) {
	    return e1->before (*e2);
    }
    
    void* operator new (size_t);
    void  operator delete (void *ptr, size_t /*size*/);
    
    static const nframes_t Immediate = 0;
    
    static void create_per_thread_pool (const std::string& n, unsigned long nitems);
    static void init_event_pool ();

private:
    static PerThreadPool* pool;
    CrossThreadPool* own_pool;

    friend class Butler;
};

class SessionEventManager {
public:
	SessionEventManager () : pending_events (2048),
		auto_loop_event(0), punch_out_event(0), punch_in_event(0) {}
	virtual ~SessionEventManager() {}

	virtual void queue_event (SessionEvent *ev) = 0;
	void clear_events (SessionEvent::Type type);

protected:
	RingBuffer<SessionEvent*> pending_events;
	typedef std::list<SessionEvent *> Events;
	Events           events;
	Events           immediate_events;
	Events::iterator next_event;

	/* there can only ever be one of each of these */

	SessionEvent *auto_loop_event;
	SessionEvent *punch_out_event;
	SessionEvent *punch_in_event;

	void dump_events () const;
	void merge_event (SessionEvent*);
	void replace_event (SessionEvent::Type, nframes64_t action_frame, nframes64_t target = 0);
	bool _replace_event (SessionEvent*);
	bool _remove_event (SessionEvent *);
	void _clear_event_type (SessionEvent::Type);

	void add_event (nframes64_t action_frame, SessionEvent::Type type, nframes64_t target_frame = 0);
	void remove_event (nframes64_t frame, SessionEvent::Type type);

	virtual void process_event(SessionEvent*) = 0;
	virtual void set_next_event () = 0;
};

} /* namespace */

#endif /* __ardour_session_event_h__ */
