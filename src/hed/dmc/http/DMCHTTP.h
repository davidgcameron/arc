// -*- indent-tabs-mode: nil -*-

#ifndef __ARC_DMCHTTP_H__
#define __ARC_DMCHTTP_H__

#include <arc/data/DMC.h>

namespace Arc {

  class DMCHTTP
    : public DMC {
  public:
    DMCHTTP(Config *cfg);
    virtual ~DMCHTTP();
    static Plugin* Instance(PluginArgument *arg);
    virtual DataPoint* iGetDataPoint(const URL& url);
  protected:
    static Logger logger;
  };

} // namespace Arc

#endif // __ARC_DMCHTTP_H__
