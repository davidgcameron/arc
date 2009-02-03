#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#ifndef WIN32
#include <sys/socket.h>
#include <arpa/inet.h>
#else
#define NOGDI
#endif

#include <iostream>
#include <fstream>
#include <vector>

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include <arc/message/PayloadStream.h>
#include <arc/message/PayloadRaw.h>
#include <arc/loader/Plugin.h>
#include <arc/message/MCCLoader.h>
#include <arc/XMLNode.h>
#include <arc/message/SecAttr.h>
#include <arc/credential/VOMSUtil.h>

#include "GlobusSigningPolicy.h"

#include "PayloadTLSStream.h"
#include "PayloadTLSMCC.h"

#include "DelegationSH.h"

#include "MCCTLS.h"

namespace Arc {

class TLSSecAttr: public Arc::SecAttr {
 friend class MCC_TLS_Service;
 friend class MCC_TLS_Client;
 public:
  TLSSecAttr(PayloadTLSStream&, ConfigTLSMCC& config, Logger& logger);
  virtual ~TLSSecAttr(void);
  virtual operator bool(void) const;
  virtual bool Export(SecAttrFormat format,XMLNode &val) const;
 protected:
  std::string identity_; // Subject of last non-proxy certificate
  std::list<std::string> subjects_; // Subjects of all certificates in chain
  std::vector<std::string> voms_attributes_; // VOMS attributes from the VOMS extension of proxy
  std::string target_; // Subject of host certificate
  virtual bool equal(const SecAttr &b) const;
};
}

unsigned int Arc::MCC_TLS::ssl_initialized_ = 0;
Glib::Mutex Arc::MCC_TLS::lock_;
Glib::Mutex* Arc::MCC_TLS::ssl_locks_ = NULL;
int Arc::MCC_TLS::ssl_locks_num_ = 0;
Arc::Logger Arc::MCC_TLS::logger(Arc::MCC::logger,"TLS");

Arc::MCC_TLS::MCC_TLS(Arc::Config& cfg,bool client) : MCC(&cfg), config_(cfg,logger,client) {
}

static Arc::Plugin* get_mcc_service(Arc::PluginArgument* arg) {
    Arc::MCCPluginArgument* mccarg =
            arg?dynamic_cast<Arc::MCCPluginArgument*>(arg):NULL;
    if(!mccarg) return NULL;
    return new Arc::MCC_TLS_Service(*(Arc::Config*)(*mccarg));
}

static Arc::Plugin* get_mcc_client(Arc::PluginArgument* arg) {
    Arc::MCCPluginArgument* mccarg =
            arg?dynamic_cast<Arc::MCCPluginArgument*>(arg):NULL;
    if(!mccarg) return NULL;
    return new Arc::MCC_TLS_Client(*(Arc::Config*)(*mccarg));
}

Arc::PluginDescriptor PLUGINS_TABLE_NAME[] = {
    { "tls.service", "HED:MCC", 0, &get_mcc_service },
    { "tls.client",  "HED:MCC", 0, &get_mcc_client  },
    { "delegation.collector", "HED:SHC", 0, &ArcSec::DelegationSH::get_sechandler},
    { NULL, NULL, 0, NULL }
};

using namespace Arc;


TLSSecAttr::TLSSecAttr(PayloadTLSStream& payload, ConfigTLSMCC& config, Logger& logger) {
   char buf[100];
   std::string subject;
   STACK_OF(X509)* peerchain = payload.GetPeerChain();
   voms_attributes_.clear();
   if(peerchain != NULL) {
      for(int idx = 0;;++idx) {
         if(idx >= sk_X509_num(peerchain)) break;
         X509* cert = sk_X509_value(peerchain,sk_X509_num(peerchain)-idx-1);
#if 0
         if(idx == 0) { // Obtain CA subject
           buf[0]=0;
           X509_NAME_oneline(X509_get_issuer_name(cert),buf,sizeof(buf));
           subject=buf;
           subjects_.push_back(subject);
         };
#endif
         buf[0]=0;
         X509_NAME_oneline(X509_get_subject_name(cert),buf,sizeof(buf));
         subject=buf;
         subjects_.push_back(subject);
#ifdef HAVE_OPENSSL_PROXY
         if(X509_get_ext_by_NID(cert,NID_proxyCertInfo,-1) < 0) {
            identity_=subject;
         };
#endif
        // Parse VOMS attributes from each certificate of the peer chain.
        bool res = parseVOMSAC(cert, config.CADir(), config.CAFile(), config.VOMSCertTrustDN(), voms_attributes_);
        if(!res) {
          logger.msg(ERROR,"VOMS attribute parsing failed");
        };
      };
   };
   X509* peercert = payload.GetPeerCert();
   if (peercert != NULL) {
      buf[0]=0;
      X509_NAME_oneline(X509_get_subject_name(peercert),buf,sizeof buf);
      subject=buf;
      //logger.msg(DEBUG, "Peer name: %s", peer_dn);
      subjects_.push_back(subject);
#ifdef HAVE_OPENSSL_PROXY
      if(X509_get_ext_by_NID(peercert,NID_proxyCertInfo,-1) < 0) {
         identity_=subject;
      };
#endif
      // Parse VOMS attributes from peer certificate
      bool res = parseVOMSAC(peercert, config.CADir(), config.CAFile(), config.VOMSCertTrustDN(), voms_attributes_);
      if(!res) {
        logger.msg(ERROR,"VOMS attribute parsing failed");
      };
      X509_free(peercert);
   };
   if(identity_.empty()) identity_=subject;

   X509* hostcert = payload.GetCert();
   if (hostcert != NULL) {
      buf[0]=0;
      X509_NAME_oneline(X509_get_subject_name(hostcert),buf,sizeof buf);
      target_=buf;
      //logger.msg(DEBUG, "Host name: %s", peer_dn);
   };
}

TLSSecAttr::~TLSSecAttr(void) {
}

TLSSecAttr::operator bool(void) const {
  return true;
}

bool TLSSecAttr::equal(const SecAttr &b) const {
  try {
    const TLSSecAttr& a = dynamic_cast<const TLSSecAttr&>(b);
    if (!a) return false;
    // ...
    return false;
  } catch(std::exception&) { };
  return false;
}

static void add_subject_attribute(XMLNode item,const std::string& subject,const char* id) {
   XMLNode attr = item.NewChild("ra:SubjectAttribute");
   attr=subject; attr.NewAttribute("Type")="string";
   attr.NewAttribute("AttributeId")=id;
}

static void Email2emailAddress(std::string& value) {
  std::string str;
  size_t found;
  while(true) {
    found =value.find("Email");
    if(found == std::string::npos) return;
    value.replace(found, 5, "emailAddress");
  };
}

bool TLSSecAttr::Export(SecAttrFormat format,XMLNode &val) const {
  if(format == UNDEFINED) {
  } else if(format == ARCAuth) {
    NS ns;
    ns["ra"]="http://www.nordugrid.org/schemas/request-arc";
    val.Namespaces(ns); val.Name("ra:Request");
    XMLNode item = val.NewChild("ra:RequestItem");
    XMLNode subj = item.NewChild("ra:Subject");
    std::list<std::string>::const_iterator s = subjects_.begin();
    std::string subject;
    if(s != subjects_.end()) {
      subject=*s;
      Email2emailAddress(subject);
      add_subject_attribute(subj,subject,"http://www.nordugrid.org/schemas/policy-arc/types/tls/ca");
      for(;s != subjects_.end();++s) {
        subject=*s;
        Email2emailAddress(subject);
        add_subject_attribute(subj,subject,"http://www.nordugrid.org/schemas/policy-arc/types/tls/chain");
      };
      Email2emailAddress(subject);
      add_subject_attribute(subj,subject,"http://www.nordugrid.org/schemas/policy-arc/types/tls/subject");
    };
    if(!identity_.empty()) {
       std::string identity(identity_);
       Email2emailAddress(identity);
       add_subject_attribute(subj,identity,"http://www.nordugrid.org/schemas/policy-arc/types/tls/identity");
    };
    if(!voms_attributes_.empty()) {
      for(int k=0; k < voms_attributes_.size(); k++) {
        add_subject_attribute(subj, voms_attributes_[k],"http://www.nordugrid.org/schemas/policy-arc/types/tls/vomsattribute");
      };
    };
    if(!target_.empty()) {
      XMLNode resource = item.NewChild("ra:Resource");
      resource=target_; resource.NewAttribute("Type")="string";
      resource.NewAttribute("AttributeId")="http://www.nordugrid.org/schemas/policy-arc/types/tls/hostidentity";
      // Following is agreed to not be use till all use cases are clarified (Bern agreement)
      //hostidentity should be SubjectAttribute, because hostidentity is be constrained to access
      //the peer delegation identity, or some resource which is attached to the peer delegation identity.
      //The constrant is defined in delegation policy.
      //add_subject_attribute(subj,target_,"http://www.nordugrid.org/schemas/policy-arc/types/tls/hostidentity");
    };
    return true;
  } else {
  };
  return false;
}

void MCC_TLS::ssl_locking_cb(int mode, int n, const char * s_, int n_) {
  if(!ssl_locks_) {
    std::cerr<<"FATAL ERROR: SSL locks not initialized"<<std::endl;
    _exit(-1);
  };
  if((n < 0) || (n >= ssl_locks_num_)) {
    std::cerr<<"FATAL ERROR: wrong SSL lock requested: "<<n<<" of "<<ssl_locks_num_<<": "<<n_<<" - "<<s_<<std::endl;
    _exit(-1);
  };
  if(mode & CRYPTO_LOCK) {
    ssl_locks_[n].lock();
  } else {
    ssl_locks_[n].unlock();
  };
}

unsigned long MCC_TLS::ssl_id_cb(void) {
  return (unsigned long)(Glib::Thread::self());
}

//void* MCC_TLS::ssl_idptr_cb(void) {
//  return (void*)(Glib::Thread::self());
//}

bool MCC_TLS::do_ssl_init(void) {
   lock_.lock();
   if(!ssl_initialized_) {
     logger.msg(VERBOSE, "MCC_TLS::do_ssl_init");     
     SSL_load_error_strings();
     if(!SSL_library_init()){
       logger.msg(ERROR, "SSL_library_init failed");
       PayloadTLSStream::HandleError(logger);
     } else {
       // We could RAND_seed() here. But since 0.9.7 OpenSSL
       // knows how to make use of OS specific source of random
       // data. I think it's better to let OpenSSL do a job.
       // Here we could also generate ephemeral DH key to avoid 
       // time consuming genaration during connection handshake.
       // But is not clear if it is needed for curently used
       // connections types at all. Needs further investigation.
       // Using RSA key violates TLS (according to OpenSSL 
       // documentation) hence we do not use it.
       //  A.K.
       int num_locks = CRYPTO_num_locks();
       if(num_locks) {
         ssl_locks_=new Glib::Mutex[num_locks];
         if(ssl_locks_) {
           ssl_locks_num_=num_locks;
           CRYPTO_set_locking_callback(&ssl_locking_cb);
           CRYPTO_set_id_callback(&ssl_id_cb);
           //CRYPTO_set_idptr_callback(&ssl_idptr_cb);
           ++ssl_initialized_;
         } else {
           logger.msg(ERROR, "Failed to allocate SSL locks");
         };
       } else {
         ++ssl_initialized_;
       };
     };
   } else {
     ++ssl_initialized_;
   };
   lock_.unlock();
   return (ssl_initialized_ != 0);
}

// This is a temporary solution because there may be other 
// components using OpenSSL.
void MCC_TLS::do_ssl_deinit(void) {
   lock_.lock();
   if(!ssl_initialized_) {
     logger.msg(ERROR, "SSL initialization counter lost sync");
   } else {
     logger.msg(VERBOSE, "MCC_TLS::do_ssl_deinit");     
     --ssl_initialized_;
     if(!ssl_initialized_) {
       CRYPTO_set_locking_callback(NULL);
       CRYPTO_set_id_callback(NULL);
       //CRYPTO_set_idptr_callback(NULL);
       if(ssl_locks_) {
         ssl_locks_num_=0;
         delete[] ssl_locks_;
         ssl_locks_=NULL;
       };
     };
   };
   lock_.unlock();
}

class MCC_TLS_Context:public MessageContextElement {
 public:
  PayloadTLSMCC* stream;
  MCC_TLS_Context(PayloadTLSMCC* s = NULL):stream(s) { };
  virtual ~MCC_TLS_Context(void) { if(stream) delete stream; };
};

/* The main functionality of the constructor method is to 
   initialize SSL layer. */
MCC_TLS_Service::MCC_TLS_Service(Arc::Config& cfg):MCC_TLS(cfg,false) {
   if(!do_ssl_init()) return;
}

MCC_TLS_Service::~MCC_TLS_Service(void) {
   do_ssl_deinit();
}

MCC_Status MCC_TLS_Service::process(Message& inmsg,Message& outmsg) {
   // Accepted payload is StreamInterface 
   // Returned payload is undefined - currently holds no information

   // TODO: probably some other credentials check is needed
   //if(!sslctx_) return MCC_Status();

   // Obtaining underlying stream
   if(!inmsg.Payload()) return MCC_Status();
   PayloadStreamInterface* inpayload = NULL;
   try {
      inpayload = dynamic_cast<PayloadStreamInterface*>(inmsg.Payload());
   } catch(std::exception& e) { };
   if(!inpayload) return MCC_Status();

   // Obtaining previously created stream context or creating a new one
   MCC_TLS_Context* context = NULL;
   {   
      MessageContextElement* mcontext = (*inmsg.Context())["tls.service"];
      if(mcontext) {
         try {
            context = dynamic_cast<MCC_TLS_Context*>(mcontext);
         } catch(std::exception& e) { };
      };
   };
   PayloadTLSMCC *stream = NULL;
   if(context) {
      // Old connection - using available SSL stream
      stream=context->stream;
   } else {
      // Creating new SSL object bound to stream of previous MCC
      // TODO: renew stream because it may be recreated by TCP MCC
      stream = new PayloadTLSMCC(inpayload,config_,logger);
      context=new MCC_TLS_Context(stream);
      inmsg.Context()->Add("tls.service",context);
   };
 
   // Creating message to pass to next MCC
   Message nextinmsg = inmsg;
   nextinmsg.Payload(stream);
   Message nextoutmsg = outmsg; nextoutmsg.Payload(NULL);

   PayloadTLSStream* tstream = dynamic_cast<PayloadTLSStream*>(stream);
   // Filling security attributes
   if(tstream && (config_.IfClientAuthn())) {
      TLSSecAttr* sattr = new TLSSecAttr(*tstream, config_, logger);
      nextinmsg.Auth()->set("TLS",sattr);
      // TODO: Remove following code, use SecAttr instead
      //Getting the subject name of peer(client) certificate
      char buf[100];     
      X509* peercert = tstream->GetPeerCert();
      if (peercert != NULL) {
         X509_NAME_oneline(X509_get_subject_name(peercert),buf,sizeof buf);
         std::string peer_dn = buf;
         logger.msg(DEBUG, "Peer name: %s", peer_dn);
         nextinmsg.Attributes()->set("TLS:PEERDN",peer_dn);
#ifdef HAVE_OPENSSL_PROXY
         if(X509_get_ext_by_NID(peercert,NID_proxyCertInfo,-1) < 0) {
            logger.msg(DEBUG, "Identity name: %s", peer_dn);
            nextinmsg.Attributes()->set("TLS:IDENTITYDN",peer_dn);
         } else {
            STACK_OF(X509)* peerchain = tstream->GetPeerChain();
            if(peerchain != NULL) {
               for(int idx = 0;;++idx) {
                  if(idx >= sk_X509_num(peerchain)) break;
                  X509* cert = sk_X509_value(peerchain,idx);
                  if(X509_get_ext_by_NID(cert,NID_proxyCertInfo,-1) < 0) {
                     X509_NAME_oneline(X509_get_subject_name(cert),buf,sizeof buf);
                     std::string identity_dn = buf;
                     logger.msg(DEBUG, "Identity name: %s", identity_dn);
                     nextinmsg.Attributes()->set("TLS:IDENTITYDN",identity_dn);
                     break;
                  };
               };
            };
         };
#else
         nextinmsg.Attributes()->set("TLS:IDENTITYDN",peer_dn);
#endif
         X509_free(peercert);
      }
   }

   // Checking authentication and authorization;
   if(!ProcessSecHandlers(nextinmsg,"incoming")) {
      logger.msg(ERROR, "Security check failed in TLS MCC for incoming message");
      return MCC_Status();
   };
   
   // Call next MCC 
   MCCInterface* next = Next();
   if(!next)  return MCC_Status();
   MCC_Status ret = next->process(nextinmsg,nextoutmsg);
   // TODO: If next MCC returns anything redirect it to stream
   if(nextoutmsg.Payload()) {
      delete nextoutmsg.Payload();
      nextoutmsg.Payload(NULL);
   };
   if(!ret) return MCC_Status();
   // For nextoutmsg, nothing to do for payload of msg, but 
   // transfer some attributes of msg
   outmsg = nextoutmsg;
   return MCC_Status(Arc::STATUS_OK);
}

MCC_TLS_Client::MCC_TLS_Client(Arc::Config& cfg):MCC_TLS(cfg,true){
   stream_=NULL;
   if(!do_ssl_init()) return;
   /* Get DN from certificate, and put it into message's attribute */
}

MCC_TLS_Client::~MCC_TLS_Client(void) {
   if(stream_) delete stream_;
   do_ssl_deinit();
}

MCC_Status MCC_TLS_Client::process(Message& inmsg,Message& outmsg) {
   // Accepted payload is Raw
   // Returned payload is Stream
   // Extracting payload
   if(!inmsg.Payload()) return MCC_Status();
   if(!stream_) return MCC_Status();
   PayloadRawInterface* inpayload = NULL;
   try {
      inpayload = dynamic_cast<PayloadRawInterface*>(inmsg.Payload());
   } catch(std::exception& e) { };
   if(!inpayload) return MCC_Status();
   //Checking authentication and authorization;
   if(!ProcessSecHandlers(inmsg,"outgoing")) {
      logger.msg(ERROR, "Security check failed in TLS MCC for outgoing message");
      return MCC_Status();
   };
   // Sending payload
   for(int n=0;;++n) {
      char* buf = inpayload->Buffer(n);
      if(!buf) break;
      int bufsize = inpayload->BufferSize(n);
      if(!(stream_->Put(buf,bufsize))) {
         logger.msg(ERROR, "Failed to send content of buffer");
         return MCC_Status();
      };
   };
   outmsg.Payload(new PayloadTLSMCC(*stream_));
   //outmsg.Attributes(inmsg.Attributes());
   //outmsg.Context(inmsg.Context());
   if(!ProcessSecHandlers(outmsg,"incoming")) {
      logger.msg(ERROR, "Security check failed in TLS MCC for incoming message");
      delete outmsg.Payload(NULL); return MCC_Status();
   };
   return MCC_Status(Arc::STATUS_OK);
}


void MCC_TLS_Client::Next(MCCInterface* next,const std::string& label) {
   if(label.empty()) {
      if(stream_) delete stream_;
      stream_=NULL;
      stream_=new PayloadTLSMCC(next,config_,logger);
   };
   MCC::Next(next,label);
}
