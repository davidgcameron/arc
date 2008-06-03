#ifndef __ARC_SUBMITTERARC0_H__
#define __ARC_SUBMITTERARC0_H__

#include <arc/client/Submitter.h>

namespace Arc {

  class ChainContext;
  class Config;

  class SubmitterARC0
    : public Submitter {

  private:
    SubmitterARC0(Config *cfg);
    ~SubmitterARC0();
    void putFiles(const std::vector< std::pair< std::string, std::string> >& fileList, std::string jobid);

  public:
    static ACC *Instance(Config *cfg, ChainContext *cxt);
    std::pair<URL, URL> Submit(Arc::JobDescription& jobdesc);
  };

} // namespace Arc

#endif // __ARC_SUBMITTERARC0_H__
