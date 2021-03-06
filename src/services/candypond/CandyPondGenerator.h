#ifndef CANDYPONDGENERATOR_H_
#define CANDYPONDGENERATOR_H_

#include <arc/data-staging/DTR.h>
#include <arc/data-staging/Scheduler.h>

#include "../a-rex/grid-manager/conf/StagingConfig.h"

namespace CandyPond {

  /// DTR Generator for CandyPond.
  class CandyPondGenerator : public DataStaging::DTRCallback {

  private:
    /// Scheduler object to process DTRs.
    DataStaging::Scheduler* scheduler;
    /// Generator state
    DataStaging::ProcessState generator_state;
    /// Whether to use the host certificate when communicating with remote delivery
    bool use_host_cert;
    /// Scratch directory used by job
    std::string scratch_dir;
    /// Whether we are running with A-REX or we manage the Scheduler ourselves
    bool run_with_arex;
    /// A-REX configuration
    const ARex::GMConfig& config;
    /// Staging configuration
    ARex::StagingConfig staging_conf;

    /// Map of job id to DTRs
    std::multimap<std::string, DataStaging::DTR_ptr> processing_dtrs;
    /// Lock for DTR map
    Arc::SimpleCondition processing_lock;
    /// Map of job id to error message, if any
    std::map<std::string, std::string> finished_jobs;
    /// Lock for finished job map
    Arc::SimpleCondition finished_lock;

    /// Logger
    static Arc::Logger logger;

  public:
    /// Start Generator and get Scheduler instance.
    /**
     * If with_arex is true then it is assumed that A-REX takes care of
     * configuring, starting and stopping the DTR Scheduler. If CandyPond
     * is run outside of A-REX then it starts an independent DTR instance,
     * using parameters given in arc.conf.
     * @param config A-REX configuration
     * @param with_arex If true then we assume A-REX starts the scheduler, if
     * false then we start and stop it.
     */
    CandyPondGenerator(const ARex::GMConfig& config, bool with_arex);

    /// Stop Scheduler if we are not running with A-REX
    ~CandyPondGenerator();

    /// Callback method to receive completed DTRs
    void receiveDTR(DataStaging::DTR_ptr dtr);

    /// Add a new request.
    /**
     * @param user User for this transfer
     * @param source Source file
     * @param destination Destination file
     * @param usercfg UserConfig with proxy information
     * @param jobid Job identifier
     * @param priority DTR priority
     */
    bool addNewRequest(const Arc::User& user,
                       const std::string& source,
                       const std::string& destination,
                       const Arc::UserConfig& usercfg,
                       const std::string& jobid,
                       int priority);

    /// Query requests for given job id.
    /**
     * @param jobid Job ID to query
     * @param error If any DTR finished with an error, the description is put
     * in error.
     * @return True if all requests for the job have finished, false otherwise
     */
    bool queryRequestsFinished(const std::string& jobid, std::string& error);
  };

} // namespace CandyPond

#endif /* CANDYPONDGENERATOR_H_ */
