/*
    SuperSonic addition (see SUPERSONIC-CHANGES.md).

    Registry of live native audio objects whose OS/driver callbacks can
    outlive them. Deregistration APIs on every platform we ship
    (AudioObjectRemovePropertyListener on macOS, COM Unregister* on
    Windows) do NOT drain callbacks already in flight, so a callback body
    can run against an instance whose destruction has begun.

    Every fenced callback body runs under this lock and bails unless its
    target instance is still registered. Deregistration takes the same
    lock, so teardown blocks until in-flight bodies finish, and any body
    starting afterwards sees the removal and does nothing.

    Deliberately headerless-namespace: this file is #included inside each
    backend's own class scope (CoreAudioClasses / WasapiClasses), so each
    translation context gets the definition where it needs it without any
    module-header wiring.
*/

class LiveObjectRegistry
{
public:
    static LiveObjectRegistry& get()
    {
        static LiveObjectRegistry registry;
        return registry;
    }

    void add (void* p)
    {
        const ScopedLock sl (lock);
        live.addIfNotAlreadyThere (p);
    }

    void remove (void* p)
    {
        const ScopedLock sl (lock);
        live.removeFirstMatchingValue (p);
    }

    template <typename Fn>
    void ifLive (void* p, Fn&& fn)
    {
        const ScopedLock sl (lock);

        if (live.contains (p))
            fn();
    }

private:
    CriticalSection lock;
    Array<void*> live;
};
