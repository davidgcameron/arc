#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <arc/security/ArcPDP/Evaluator.h>
#include <arc/security/ArcPDP/EvaluatorLoader.h>
/*
#include <iostream>
#include <fstream>

#include <arc/loader/PDPLoader.h>
#include <arc/XMLNode.h>
#include <arc/Thread.h>
#include <arc/ArcConfig.h>
#include <arc/ArcLocation.h>
#include <arc/Logger.h>
#include <arc/security/ArcPDP/Response.h>
#include <arc/security/ArcPDP/attr/AttributeValue.h>
#include <arc/security/ArcPDP/EvaluatorLoader.h>
*/

#include "GACLPDP.h"

Arc::Logger ArcSec::GACLPDP::logger(Arc::Logger::getRootLogger(), "ArcSec.GACLPDP");

Arc::SecAttrFormat ArcSec::GACLPDP::GACL("gacl");

/*
static ArcSec::PDP* get_pdp(Arc::Config *cfg,Arc::ChainContext *ctx) {
    return new ArcSec::ArcPDP(cfg);
}

pdp_descriptors ARC_PDP_LOADER = {
    { "gacl.pdp", 0, &get_pdp},
    { NULL, 0, NULL }
};
*/

using namespace Arc;

namespace ArcSec {

Plugin* GACLPDP::get_gacl_pdp(PluginArgument* arg) {
    PDPPluginArgument* pdparg =
            arg?dynamic_cast<PDPPluginArgument*>(arg):NULL;
    if(!pdparg) return NULL;
    return new GACLPDP((Config*)(*pdparg),arg);
}

class GACLPDPContext:public Arc::MessageContextElement {
 friend class GACLPDP;
 private:
  Evaluator* eval;
 public:
  GACLPDPContext(Evaluator* e);
  GACLPDPContext(void);
  virtual ~GACLPDPContext(void);
};

GACLPDPContext::~GACLPDPContext(void) {
  if(eval) delete eval;
}

GACLPDPContext::GACLPDPContext(Evaluator* e):eval(e) {
}

GACLPDPContext::GACLPDPContext(void):eval(NULL) {
  EvaluatorLoader eval_loader;
  eval = eval_loader.getEvaluator(std::string("gacl.evaluator"));
}

GACLPDP::GACLPDP(Config* cfg, Arc::PluginArgument* parg):PDP(cfg,parg) {
  XMLNode pdp_node(*cfg);

  XMLNode filter = (*cfg)["Filter"];
  if((bool)filter) {
    XMLNode select_attr = filter["Select"];
    XMLNode reject_attr = filter["Reject"];
    for(;(bool)select_attr;++select_attr) select_attrs.push_back((std::string)select_attr);
    for(;(bool)reject_attr;++reject_attr) reject_attrs.push_back((std::string)reject_attr);
  };
  XMLNode policy_store = (*cfg)["PolicyStore"];
  XMLNode policy_location = policy_store["Location"];
  for(;(bool)policy_location;++policy_location) policy_locations.push_back((std::string)policy_location);
  XMLNode policy_doc = policy_store["Policy"];
  for(;(bool)policy_doc;++policy_doc) policy_docs.AddNew(policy_doc);
}

PDPStatus GACLPDP::isPermitted(Message *msg) const{
  Evaluator* eval = NULL;

  std::string ctxid = "arcsec.gaclpdp";
  try {
    Arc::MessageContextElement* mctx = (*(msg->Context()))[ctxid];
    if(mctx) {
      GACLPDPContext* pdpctx = dynamic_cast<GACLPDPContext*>(mctx);
      if(pdpctx) {
        eval=pdpctx->eval;
      };
    };
  } catch(std::exception& e) { };
  if(!eval) {
    GACLPDPContext* pdpctx = new GACLPDPContext();
    if(pdpctx) {
      eval=pdpctx->eval;
      if(eval) {
        for(std::list<std::string>::const_iterator it = policy_locations.begin(); it!= policy_locations.end(); it++) {
          eval->addPolicy(SourceFile(*it));
        }
        for(int n = 0;n<policy_docs.Size();++n) {
          eval->addPolicy(Source(const_cast<Arc::XMLNodeContainer&>(policy_docs)[n]));
        }
        msg->Context()->Add(ctxid, pdpctx);
      } else {
        delete pdpctx;
      }
    }
    if(!eval) logger.msg(ERROR, "Can not dynamically produce Evaluator");
  }
  if(!eval) {
    logger.msg(ERROR,"Evaluator for GACLPDP was not loaded"); 
    return false;
  };

  MessageAuth* mauth = msg->Auth()->Filter(select_attrs,reject_attrs);
  MessageAuth* cauth = msg->AuthContext()->Filter(select_attrs,reject_attrs);
  if((!mauth) && (!cauth)) {
    logger.msg(ERROR,"Missing security object in message");
    return false;
  };
  NS ns;
  XMLNode requestxml(ns,"");
  if(mauth) {
    if(!mauth->Export(GACL,requestxml)) {
      delete mauth;
      logger.msg(ERROR,"Failed to convert security information to ARC request");
      return false;
    };
    delete mauth;
  };
  if(cauth) {
    if(!cauth->Export(GACL,requestxml)) {
      delete mauth;
      logger.msg(ERROR,"Failed to convert security information to ARC request");
      return false;
    };
    delete cauth;
  };
  if(DEBUG >= logger.getThreshold()) {
    std::string s;
    requestxml.GetXML(s);
    logger.msg(DEBUG,"GACL Auth. request: %s",s);
  };
  if(requestxml.Size() <= 0) {
    logger.msg(ERROR,"No requested security information was collected");
    return false;
  };

  //Call the evaluation functionality inside Evaluator
  Response *resp = eval->evaluate(requestxml);
  if(!resp) return false;
  ResponseList rlist = resp->getResponseItems();

  // Current implementation of GACL Evaluator returns only one item
  // and only PERMIT/DENY results.
  if(rlist.size() <= 0) { delete resp; return false; };
  ResponseItem* item = rlist[0];
  if(item->res != DECISION_PERMIT) { delete resp; return false; };
  delete resp;
  return true;
}

GACLPDP::~GACLPDP(){
}

} // namespace ArcSec

