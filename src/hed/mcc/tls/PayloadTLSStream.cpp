#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#ifndef WIN32
#include <sys/poll.h>
#else
#define NOGDI
#include <objbase.h>
#include <io.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "PayloadTLSStream.h"

namespace ArcMCCTLS {

void PayloadTLSStream::HandleError(int code) {
   HandleError(logger_,code);
}

void PayloadTLSStream::HandleError(Logger& logger,int code) {
  unsigned long e = (code==SSL_ERROR_NONE)?ERR_get_error():code;
  while(e != SSL_ERROR_NONE) {
    if(e == SSL_ERROR_SYSCALL) {
      // Hiding system errors
      // int err = errno;
      // logger.msg(DEBUG, "SSL error: system call failed: %d, %s",err,StrError(err));
    } else {
      const char* lib = ERR_lib_error_string(e);
      const char* func = ERR_func_error_string(e);
      const char* reason = ERR_reason_error_string(e);
      logger.msg(DEBUG, "SSL error: %d - %s:%s:%s",
                        e,
                        lib?lib:"",
                        func?func:"",
                        reason?reason:"");
    };
    e = ERR_get_error();
  }
}

void PayloadTLSStream::ClearError(void) {
  int e = ERR_get_error();
  while(e != 0) {
    e = ERR_get_error();
  }
}

PayloadTLSStream::PayloadTLSStream(Logger& logger,SSL* ssl):timeout_(0),ssl_(ssl),logger_(logger) {
  //initialize something
  return;
}

PayloadTLSStream::PayloadTLSStream(PayloadTLSStream& stream):timeout_(stream.timeout_),ssl_(stream.ssl_),logger_(stream.logger_) {
}

PayloadTLSStream::~PayloadTLSStream(void) {
  ClearError();
}

bool PayloadTLSStream::Get(char* buf,int& size) {
  if(ssl_ == NULL) return false;
  //ssl read
  ssize_t l=size;
  size=0;
  l=SSL_read(ssl_,buf,l);
  if(l <= 0){
     HandleError(SSL_get_error(ssl_,l));
     return false;
  }
  size=l;
  return true;
}

bool PayloadTLSStream::Get(std::string& buf) {
  char tbuf[1024];
  int l = sizeof(tbuf);
  bool result = Get(tbuf,l);
  buf.assign(tbuf,l);
  return result;
}

bool PayloadTLSStream::Put(const char* buf,Size_t size) {
  //ssl write
  ssize_t l;
  if(ssl_ == NULL) return false;
  for(;size;){
     l=SSL_write(ssl_,buf,size);
     if(l <= 0){
        HandleError(SSL_get_error(ssl_,l));
        return false;
     }
     buf+=l; size-=l;
  }
  return true;
}

X509* PayloadTLSStream::GetPeerCert(void){
  X509* peercert;
  int err;
  if(ssl_ == NULL) return NULL;
  if((err=SSL_get_verify_result(ssl_)) == X509_V_OK){
    peercert=SSL_get_peer_certificate (ssl_);
    if(peercert!=NULL) return peercert;
    logger_.msg(ERROR,"Peer certificate cannot be extracted");
    HandleError();
  }
  else{
    logger_.msg(ERROR,"Peer cert verification fail");
    logger_.msg(ERROR,"%s",X509_verify_cert_error_string(err));
    HandleError(err);
  }
  return NULL;
}

X509* PayloadTLSStream::GetCert(void){
  X509* cert;
  if(ssl_ == NULL) return NULL;
  cert=SSL_get_certificate (ssl_);
  if(cert!=NULL) return cert;
  logger_.msg(VERBOSE,"Certificate cannot be extracted, make sure it is the case where client side authentication is turned off");
  HandleError();
  return NULL;
}

STACK_OF(X509)* PayloadTLSStream::GetPeerChain(void){
  STACK_OF(X509)* peerchain;
  int err;
  if(ssl_ == NULL) return NULL;
  if((err=SSL_get_verify_result(ssl_)) == X509_V_OK){
    peerchain=SSL_get_peer_cert_chain (ssl_);
    if(peerchain!=NULL) return peerchain;
    logger_.msg(ERROR,"Peer certificate chain cannot be extracted");
    HandleError();
  }
  else{
    logger_.msg(ERROR,"Peer cert verification fail");
    logger_.msg(ERROR,"%s",X509_verify_cert_error_string(err));
    HandleError(err);
  }
  return NULL;
}

} // namespace Arc
