#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <iostream>
#include <signal.h>

#include <arc/ArcConfig.h>
#include <arc/Logger.h>
#include <arc/message/SOAPEnvelope.h>
#include <arc/message/PayloadSOAP.h>
#include <arc/message/MCC.h>
#include <arc/message/MCCLoader.h>
#ifdef WIN32
#include <arc/win32.h>
#endif

int main(void) {
  signal(SIGTTOU,SIG_IGN);
  signal(SIGTTIN,SIG_IGN);
  Arc::Logger logger(Arc::Logger::rootLogger, "Test");
  Arc::LogStream logcerr(std::cerr);
  Arc::Logger::rootLogger.addDestination(logcerr);
  logger.msg(Arc::INFO, "Creating client side chain");
  // Create client chain
  Arc::XMLNode client_doc("\
    <ArcConfig\
      xmlns=\"http://www.nordugrid.org/schemas/ArcConfig/2007\"\
      xmlns:tcp=\"http://www.nordugrid.org/schemas/ArcMCCTCP/2007\">\
     <ModuleManager>\
        <Path>.libs/</Path>\
        <Path>../../hed/mcc/http/.libs/</Path>\
        <Path>../../hed/mcc/soap/.libs/</Path>\
        <Path>../../hed/mcc/tls/.libs/</Path>\
        <Path>../../hed/mcc/tcp/.libs/</Path>\
        <Path>../../hed/shc/.libs/</Path>\
     </ModuleManager>\
     <Plugins><Name>mcctcp</Name></Plugins>\
     <Plugins><Name>mcctls</Name></Plugins>\
     <Plugins><Name>mcchttp</Name></Plugins>\
     <Plugins><Name>mccsoap</Name></Plugins>\
     <Plugins><Name>arcshc</Name></Plugins>\
     <Chain>\
      <Component name='tcp.client' id='tcp'><tcp:Connect><tcp:Host>127.0.0.1</tcp:Host><tcp:Port>60000</tcp:Port></tcp:Connect></Component>\
      <Component name='tls.client' id='tls'><next id='tcp'/>\
        <!--For proxy certificate, KeyPath and CertificatePath are supposed to be the same-->\
        <KeyPath>../echo/testkey-nopass.pem</KeyPath>\
        <CertificatePath>../echo/testcert.pem</CertificatePath>\
        <CACertificatePath>../echo/cacert.pem</CACertificatePath>\
      </Component>\
      <Component name='http.client' id='http'><next id='tls'/>\
        <Method>POST</Method>\
        <Endpoint>/echo</Endpoint>\
      </Component>\
      <Component name='soap.client' id='soap' entry='soap'>\
        <next id='http'/>\
        <SecHandler name='delegation.handler' id='delegation' event='outgoing'>\
          <Type>x509</Type>\
          <Role>client</Role>\
          <!--DelegationServiceEndpoint>https://glueball.uio.no:60000/delegation</DelegationServiceEndpoint-->\
          <DelegationServiceEndpoint>https://127.0.0.1:60000/delegation</DelegationServiceEndpoint>\
          <PeerServiceEndpoint>https://127.0.0.1:60000/echo</PeerServiceEndpoint>\
          <KeyPath>../echo/testkey-nopass.pem</KeyPath>\
          <CertificatePath>../echo/testcert.pem</CertificatePath>\
          <!--ProxyPath>/tmp/5612d050.pem</ProxyPath-->\
          <!--DelegationCredIdentity>/O=KnowARC/OU=UiO/CN=squark.uio.no</DelegationCredIdentity-->\
          <!--CACertificatePath>../echo/cacert.pem</CACertificatePath-->\
          <CACertificatesDir>../echo/certificates</CACertificatesDir>\
        </SecHandler>\
      </Component>\
     </Chain>\
    </ArcConfig>");

  /*For the firstly client in the service invocation chain, the credential path
    should be configured for the 'client' role delegation handler.
     <KeyPath>../echo/testkey-nopass.pem</KeyPath>\
     <CertificatePath>../echo/testcert.pem</CertificatePath>\
     <!--ProxyPath>/tmp/5612d050.pem</ProxyPath-->\
    Alternatively, For the clients which are called in the intermediate 
    service inside the service invocation chain, the the 'Identity' should 
    be configured for the 'client' role delegation handler. The 'Identity' 
    can be parsed from the 'incoming' message context of the service itself 
    by service implementation: 
      std::string identity= msg->Attributes()->get("TLS:IDENTITYDN");
    Afterwards, the service implementation should change the client 
    (the client that this service will call to contact the next intemediate service) 
    configuration to add 'DelegationCredIdentity' like the following.
    <DelegationCredIdentity>/O=KnowARC/OU=UiO/CN=squark.uio.no</DelegationCredIdentity>\
  */

  Arc::Config client_config(client_doc);
  if(!client_config) {
    logger.msg(Arc::ERROR, "Failed to load client configuration");
    return -1;
  };
  Arc::MCCLoader client_loader(client_config);
  logger.msg(Arc::INFO, "Client side MCCs are loaded");
  Arc::MCC* client_entry = client_loader["soap"];
  if(!client_entry) {
    logger.msg(Arc::ERROR, "Client chain does not have entry point");
    return -1;
  };

  // for (int i = 0; i < 10; i++) {
  // Create and send echo request
  logger.msg(Arc::INFO, "Creating and sending request");
  Arc::NS echo_ns; echo_ns["echo"]="urn:echo";
  Arc::PayloadSOAP req(echo_ns);
  req.NewChild("echo").NewChild("say")="HELLO";
  Arc::Message reqmsg;
  Arc::Message repmsg;
  reqmsg.Payload(&req);
  // It is a responsibility of code initiating first Message to
  // provide Context and Attributes as well.
  Arc::MessageAttributes attributes_req;
  Arc::MessageAttributes attributes_rep;
  Arc::MessageContext context;
  reqmsg.Attributes(&attributes_req);
  reqmsg.Context(&context);
  repmsg.Attributes(&attributes_rep);
  repmsg.Context(&context);
//  for(int i=0; i<1; i++) {
    Arc::MCC_Status status = client_entry->process(reqmsg,repmsg);
    if(!status) {
      logger.msg(Arc::ERROR, "Request failed");
      std::cerr << "Status: " << std::string(status) << std::endl;
      return -1;
    };
    Arc::PayloadSOAP* resp = NULL;
    if(repmsg.Payload() == NULL) {
      logger.msg(Arc::ERROR, "There is no response");
      return -1;
    };
    try {
      resp = dynamic_cast<Arc::PayloadSOAP*>(repmsg.Payload());
    } catch(std::exception&) { };
    if(resp == NULL) {
      logger.msg(Arc::ERROR, "Response is not SOAP");
      return -1;
    };
    std::string xml;
    resp->GetXML(xml);
    std::cout << "XML: "<< xml << std::endl;
    std::cout << "Response: " << (std::string)((*resp)["echoResponse"]["hear"]) << std::endl;
    if(resp) delete resp;
//  }    
  return 0;
}
