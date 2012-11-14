#include <arc/StringConv.h>

#include "ConfigUtils.h"
#include "ConfigSections.h"

#include "StagingConfig.h"

namespace ARex {

Arc::Logger StagingConfig::logger(Arc::Logger::getRootLogger(), "StagingConfig");

StagingConfig::StagingConfig(const GMConfig& config):
  max_delivery(-1),
  max_processor(-1),
  max_emergency(-1),
  max_prepared(-1),
  min_speed(0),
  min_speed_time(300),
  min_average_speed(0),
  max_inactivity_time(300),
  max_retries(10),
  passive(false),
  secure(false),
  remote_size_limit(0),
  use_host_cert_for_remote_delivery(false),
  log_level(Arc::Logger::getRootLogger().getThreshold()),
  valid(true)
{

  // For ini-style, use [data-staging] section, for xml use <dataTransfer> node

  // Fill from GMConfig, then override those values with values in data-staging conf
  fillFromGMConfig(config);

  std::ifstream cfile;
  if (!config_open(cfile, config.ConfigFile())) {
    logger.msg(Arc::ERROR, "Can't read configuration file");
    valid = false;
    return;
  }
  // detect type of file
  switch(config_detect(cfile)) {
    case config_file_XML: {
      Arc::XMLNode cfg;
      if (!cfg.ReadFromStream(cfile)) {
        logger.msg(Arc::ERROR, "Can't interpret configuration file as XML");
        valid = false;
      }
      else if (!readStagingConf(cfg)) {
        logger.msg(Arc::ERROR, "Configuration error");
        valid = false;
      }
    } break;

    case config_file_INI: {
      if (!readStagingConf(cfile)) {
        logger.msg(Arc::ERROR, "Configuration error");
        valid = false;
      }
    } break;

    default: {
      logger.msg(Arc::ERROR, "Can't recognize type of configuration file");
      valid = false;
    } break;
  }
  config_close(cfile);
}

void StagingConfig::fillFromGMConfig(const GMConfig& config) {

  max_delivery = config.max_jobs_staging;
  max_emergency = config.max_jobs_staging_emergency;
  if (max_delivery > 0 && config.max_downloads > 0) max_delivery *= config.max_downloads;
  max_processor = max_delivery;
  if (max_emergency > 0 && config.max_downloads > 0) max_emergency *= config.max_downloads;
  min_speed = config.min_speed;
  min_speed_time = config.min_speed_time;
  min_average_speed = config.min_average_speed;
  max_inactivity_time = config.max_inactivity_time;
  passive = config.use_passive_transfer;
  secure = config.use_secure_transfer;
  max_retries = config.max_retries;
  preferred_pattern = config.preferred_pattern;
  share_type = config.share_type;
  defined_shares = config.limited_share;
}

bool StagingConfig::readStagingConf(std::ifstream& cfile) {

  ConfigSections cf(cfile);
  cf.AddSection("data-staging");
  for(;;) {
    std::string rest;
    std::string command;
    cf.ReadNext(command,rest);
    if (command.empty()) break; // eof

    if (command == "maxdelivery") {
      if (!paramToInt(config_next_arg(rest), max_delivery)) {
        logger.msg(Arc::ERROR, "Bad number in maxdelivery");
        return false;
      }
    }
    else if (command == "maxemergency") {
      if (!paramToInt(config_next_arg(rest), max_emergency)) {
        logger.msg(Arc::ERROR, "Bad number in maxemergency");
        return false;
      }
    }
    else if (command == "maxprocessor") {
      if (!paramToInt(config_next_arg(rest), max_processor)) {
        logger.msg(Arc::ERROR, "Bad number in maxprocessor");
        return false;
      }
    }
    else if (command == "maxprepared") {
      if (!paramToInt(config_next_arg(rest), max_prepared) || max_prepared <= 0) {
        logger.msg(Arc::ERROR, "Bad number in maxprepared");
        return false;
      }
    }
    else if (command == "maxtransfertries") {
      if (!paramToInt(config_next_arg(rest), max_retries)) {
        logger.msg(Arc::ERROR, "Bad number in maxtransfertries");
        return false;
      }
    }
    else if (command == "speedcontrol") {
      if (!Arc::stringto(config_next_arg(rest), min_speed) ||
          !Arc::stringto(config_next_arg(rest), min_speed_time) ||
          !Arc::stringto(config_next_arg(rest), min_average_speed) ||
          !Arc::stringto(config_next_arg(rest), max_inactivity_time)) {
        logger.msg(Arc::ERROR, "Bad number in speedcontrol");
        return false;
      }
    }
    else if (command == "sharetype") {
      share_type = config_next_arg(rest);
    }
    else if (command == "definedshare") {
      std::string share = config_next_arg(rest);
      int priority = 0;
      if (!paramToInt(config_next_arg(rest), priority) || priority <= 0) {
        logger.msg(Arc::ERROR, "Bad number in definedshare %s", share);
        return false;
      }
      defined_shares[share] = priority;
    }
    else if (command == "deliveryservice") {
      std::string url = config_next_arg(rest);
      Arc::URL u(url);
      if (!u) {
        logger.msg(Arc::ERROR, "Bad URL in deliveryservice: %s", url);
        return false;
      }
      delivery_services.push_back(u);
    }
    else if (command == "localdelivery") {
      std::string use_local = config_next_arg(rest);
      if (use_local == "yes") delivery_services.push_back(Arc::URL("file:/local"));
    }
    else if (command == "remotesizelimit") {
      if (!Arc::stringto(config_next_arg(rest), remote_size_limit)) {
        logger.msg(Arc::ERROR, "Bad number in remotesizelimit");
        return false;
      }
    }
    else if (command == "passivetransfer") {
      std::string pasv = config_next_arg(rest);
      if (pasv == "yes") passive = true;
    }
    else if (command == "securetransfer") {
      std::string sec = config_next_arg(rest);
      if (sec == "yes") secure = true;
    }
    else if (command == "preferredpattern") {
      preferred_pattern = config_next_arg(rest);
    }
    else if (command == "usehostcert") {
      std::string use_host_cert = config_next_arg(rest);
      if (use_host_cert == "yes") use_host_cert_for_remote_delivery = true;
    }
    else if (command == "debug") {
      unsigned int level;
      if (!Arc::strtoint(config_next_arg(rest), level)) {
        logger.msg(Arc::ERROR, "Bad value for debug");
        return false;
      }
      log_level = Arc::old_level_to_level(level);
    }
    else if (command == "dtrlog") {
      dtr_log = config_next_arg(rest);
    }
  }
  return true;
}

bool StagingConfig::readStagingConf(const Arc::XMLNode& cfg) {
  /*
  DTR
    maxDelivery
    maxProcessor
    maxEmergency
    maxPrepared
    shareType
    definedShare
      name
      priority
    deliveryService
    localDelivery
    useHostCert
    logLevel
    dtrLog
  */
  Arc::XMLNode tmp_node = cfg["dataTransfer"]["DTR"];
  if (tmp_node) {
    if (!elementtoint(tmp_node, "maxDelivery", max_delivery, &logger)) return false;
    if (!elementtoint(tmp_node, "maxProcessor", max_processor, &logger)) return false;
    if (!elementtoint(tmp_node, "maxEmergency", max_emergency, &logger)) return false;
    if (!elementtoint(tmp_node, "maxPrepared", max_prepared, &logger)) return false;
    if (tmp_node["shareType"]) share_type = (std::string)tmp_node["shareType"];
    Arc::XMLNode defined_share = tmp_node["definedShare"];
    for(;defined_share;++defined_share) {
      std::string share_name = defined_share["name"];
      int share_priority = -1;
      if (elementtoint(defined_share, "priority", share_priority, &logger) &&
          (share_priority > 0) && !share_name.empty()) {
        defined_shares[share_name] = share_priority;
      }
    }
    Arc::XMLNode delivery_service = tmp_node["deliveryService"];
    for(;delivery_service;++delivery_service) {
      Arc::URL u((std::string)delivery_service);
      if (!u) {
        logger.msg(Arc::ERROR, "Bad URL in deliveryService: %s", std::string(delivery_service));
        return false;
      }
      delivery_services.push_back(u);
    }
    bool use_local_delivery = false;
    if (!elementtobool(tmp_node,"localDelivery",use_local_delivery,&logger)) return false;
    if (use_local_delivery) delivery_services.push_back(Arc::URL("file:/local"));
    if (tmp_node["remoteSizeLimit"]) {
      if (!Arc::stringto((std::string)tmp_node["remoteSizeLimit"], remote_size_limit)) return false;
    }
    if (!elementtobool(tmp_node, "localDelivery", use_host_cert_for_remote_delivery, &logger)) return false;
    if (tmp_node["logLevel"]) {
      if (!Arc::string_to_level((std::string)tmp_node["logLevel"], log_level)) {
        logger.msg(Arc::ERROR, "Bad value for logLevel");
        return false;
      }
    }
    if (tmp_node["dtrLog"]) dtr_log = (std::string)tmp_node["dtrLog"];
  }

  return true;
}

bool StagingConfig::paramToInt(const std::string& param, int& value) {

  int i;
  if (!Arc::stringto(param, i)) return false;
  if (i < 0) i=-1;
  value = i;
  return true;
}

} // namespace ARex