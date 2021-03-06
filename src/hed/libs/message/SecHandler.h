#ifndef __ARC_SEC_SECHANDLER_H__
#define __ARC_SEC_SECHANDLER_H__

#include <arc/ArcConfig.h>
#include <arc/message/Message.h>
#include <arc/loader/Plugin.h>
#include <arc/Logger.h>

namespace Arc {
class ChainContext;
}

namespace ArcSec {


  class SecHandlerStatus {
   public:
    enum {
      STATUS_ALLOW = 0,
      STATUS_DENY = 1
    } Code;

    SecHandlerStatus(void);

    SecHandlerStatus(bool positive);

    SecHandlerStatus(int code);

    SecHandlerStatus(int code, const std::string& explanation);

    SecHandlerStatus(int code,
               const std::string& origin,
               const std::string& explanation);

    operator bool(void) const { return (code == 0); };

    int getCode(void) const;

    const std::string& getOrigin(void) const;

    const std::string& getExplanation(void) const;

    operator std::string(void) const;

   private:
    //! The code status. 0 always stands for positive.
    int code;

    //! A string describing the origin SHC of this object.
    std::string origin;

    //! A user-friendly explanation of this object.
    std::string explanation;
  };

  /// Base class for simple security handling plugins
  /** This virtual class defines method Handle() which processes 
    security related information/attributes in Message and optionally
    makes security decision. Instances of such classes are normally
    arranged in chains abd are called on incoming and outgoing messages
    in various MCC and Service plugins. Return value of Handle() defines
    either processing should continie (true) or stop with error (false). 
    Configuration of SecHandler is consumed during creation of instance
    through XML subtree fed to constructor. */ 
  class SecHandler: public Arc::Plugin {
   public:
    SecHandler(Arc::Config*, Arc::PluginArgument* arg):Arc::Plugin(arg) {};
    virtual ~SecHandler() {};
    virtual SecHandlerStatus Handle(Arc::Message *msg) const = 0;

   protected:
    static Arc::Logger logger;
  };

  #define SecHandlerPluginKind ("HED:SHC")

  class SecHandlerPluginArgument: public Arc::PluginArgument {
   private:
    Arc::Config* config_;
    Arc::ChainContext* context_;
   public:
    SecHandlerPluginArgument(Arc::Config* config,Arc::ChainContext* context):config_(config),context_(context) { };
    virtual ~SecHandlerPluginArgument(void) { };
    operator Arc::Config* (void) { return config_; };
    operator Arc::ChainContext* (void) { return context_; };
  };

  /** Helper class to create Security Handler configuration */
  class SecHandlerConfig: public Arc::XMLNode {
   public:
    SecHandlerConfig(const std::string& name,const std::string& event = "",const std::string& id = "");
  };

} // namespace ArcSec

#endif /* __ARC_SEC_SECHANDLER_H__ */
