// -*- indent-tabs-mode: nil -*-

#ifndef __ARC_TARGETINFORMATIONRETRIEVERLDAPNG_H__
#define __ARC_TARGETINFORMATIONRETRIEVERLDAPNG_H__

#include <list>

#include <arc/client/TargetInformationRetriever.h>

namespace Arc {

  class Logger;
  class EndpointQueryingStatus;
  class ExecutionTarget;
  class UserConfig;

  class TargetInformationRetrieverPluginLDAPNG : public TargetInformationRetrieverPlugin {
  public:
    TargetInformationRetrieverPluginLDAPNG() { supportedInterfaces.push_back("org.nordugrid.ldapng"); };
    ~TargetInformationRetrieverPluginLDAPNG() {};
    static Plugin* Instance(PluginArgument *) { return new TargetInformationRetrieverPluginLDAPNG(); };

    virtual EndpointQueryingStatus Query(const UserConfig&, const ComputingInfoEndpoint&, std::list<ExecutionTarget>&, const EndpointFilter<ExecutionTarget>&) const;

  private:
    static Logger logger;
  };

} // namespace Arc

#endif // __ARC_TARGETINFORMATIONRETRIEVERLDAPNG_H__