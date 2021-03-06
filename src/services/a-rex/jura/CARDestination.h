#ifndef CARDESTINATION_H
#define CARDESTINATION_H

#include "Destination.h"
#include "JobLogFile.h"
#include "Config.h"
#include <stdexcept>
#include <string>
#include <map>
#include <arc/XMLNode.h>
#include <arc/communication/ClientInterface.h>
#include <arc/message/MCC.h>
#include <arc/message/MCCLoader.h>
#include <arc/message/Message.h>
#include <arc/message/PayloadSOAP.h>

namespace ArcJura
{

  /** Reporting destination adapter for EMI. */
  class CARDestination:public Destination
  {
  private:
    Arc::Logger logger;
    Arc::MCCConfig cfg;
    const Config::APEL conf;
    Arc::URL service_url;
    std::string output_dir;
    /** Max number of URs to put in a set before submitting it */
    int max_ur_set_size;
    /** Actual number of usage records in set */
    int urn;
    /** List of copies of job logs */
    std::list<JobLogFile> joblogs;
    /** Usage Record set XML */
    Arc::XMLNode usagerecordset;
    
    int submit_batch();
    Arc::MCC_Status send_request(const std::string &urset);
    void clear();

  public:
    /** Constructor. Service URL and CAR-related parameters (e.g. UR
     *  batch size) are extracted from the given job log file.
     */
    CARDestination(JobLogFile& joblog, const Config::APEL &conf);
    /** Generates record from job log file content, collects it into the
     *  UR batch, and if batch is full, submits it to the service. 
     */
    void report(JobLogFile& joblog);
    void finish();
    ~CARDestination();
  };

}

#endif
