#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "Thread.h"
#include "Logger.h"

namespace Arc {

static Logger threadLogger(Logger::getRootLogger(), "Thread");

class ThreadInitializer {
public:
  ThreadInitializer(void) {
    threadLogger.msg(INFO, "Initialize thread system");
    if(!Glib::thread_supported()) Glib::thread_init();
  };
};

class ThreadArgument {
    public:
        void* arg;
        void (*func)(void*);
        ThreadArgument(void (*f)(void*), void* a):arg(a),func(f) { }
        ~ThreadArgument(void) { };
        void thread(void) { (*func)(arg); delete this; };
};

// TODO: do something to make sure this static object is 
// initialized before any other static object which uses 
// mutexes, semaphors, etc.
static ThreadInitializer thread_initializer;

bool CreateThreadFunction(void (*func)(void*), void* arg) {
    ThreadArgument* argument = new ThreadArgument(func,arg);
    try {
        // TODO. Who is going to destroy created object? Check for memory leaks.
        Glib::Thread::create(sigc::mem_fun(*argument,&ThreadArgument::thread),
                             false);
    } catch(std::exception& e) { 
      threadLogger.msg(ERROR, e.what());
      delete argument;
      return false;
    };
    return true;
}

/*
 Example of how to use CreateThreadClass macro

class testclass {
    public:
        int a;
        testclass(int v) { a=v; };
        void run(void) { a=0; };
};

void test(void) {
   testclass tc(1);
   CreateThreadClass(tc,testclass::run);
}
*/

} // namespace Arc
