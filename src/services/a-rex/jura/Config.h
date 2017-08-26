#ifndef _JURA_CONFIG_H
#define _JURA_CONFIG_H

#include <cmath>
#include <string>
#include <vector>
#include <arc/URL.h>
#include <arc/Logger.h>

namespace ArcJura
{
  class Config
  {
  public:
    Config(char const * configFile);
    operator bool() { return processed; }
    bool operator!() { return !processed; }

    std::string const & getLogfile() const { return logfile; }
    Arc::LogLevel getLoglevel() const { return loglevel; }
    std::string const & getArchiving() const { return archiving; }
    unsigned int getArchivettl() const { return archivettl; }
    unsigned int getUrbatchsize() const { return urbatchsize; }
    std::string const & getVOMSlessVO() const { return vomsless_vo; }
    std::string const & getVOMSlessIssuer() const { return vomsless_issuer; }
    std::string const & getVOGroup() const { return vo_group; }
    unsigned int getUrdeliveryKeepfaild() const { return urdelivery_keepfailed; }
    unsigned int getUrdeliveryFrequency() const { return urdelivery_frequency; }

    class SGAS {
    public:
      static unsigned int const default_urbatchsize;
      SGAS(): urbatchsize(default_urbatchsize) {}
      std::string name;
      Arc::URL targeturl;
      std::string localid_prefix;
      std::vector<std::string> vofilters;
      unsigned int urbatchsize;
    };

    std::vector<SGAS> const getSGAS() const { return sgas_entries; }

    class APEL {
    public:
      static unsigned int const default_urbatchsize;
      APEL(): benchmark_value(NAN), use_ssl(false), urbatchsize(default_urbatchsize) {}
      std::string name;
      Arc::URL targeturl;
      std::string topic;
      std::string gocdb_name;
      std::string benchmark_type;
      float  benchmark_value;
      std::string benchmark_description; 
      bool use_ssl;
      std::vector<std::string> vofilters;
      unsigned int urbatchsize;
    };

    std::vector<APEL> const getAPEL() const { return apel_entries; }

  private:
    bool processed;

    static char const * const default_logfile;
    static Arc::LogLevel const default_loglevel;
    static char const * const default_archiving;
    static unsigned int const default_archivettl;
    static unsigned int const default_urbatchsize;
    static unsigned int const default_urdelivery_keepfailed;
    static unsigned int const default_urdelivery_frequency;

    std::string logfile;
    Arc::LogLevel loglevel;
    std::string archiving;
    unsigned int archivettl;
    unsigned int urbatchsize;
    std::string vomsless_vo;
    std::string vomsless_issuer;
    std::string vo_group;
    unsigned int urdelivery_keepfailed;
    unsigned int urdelivery_frequency;
    std::vector<SGAS> sgas_entries;
    std::vector<APEL> apel_entries;

  };

}

#endif
