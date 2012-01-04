#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <math.h>

#include <set>

#include <arc/FileUtils.h>
#include <arc/Utils.h>

#include "Scheduler.h"
#include "DataDeliveryRemoteComm.h"

namespace DataStaging {
	
  Arc::Logger Scheduler::logger(Arc::Logger::getRootLogger(), "DataStaging.Scheduler");
  
  Scheduler::Scheduler(): scheduler_state(INITIATED) {
    // Conservative defaults
    PreProcessorSlots = 20;
    DeliverySlots = 10;
    PostProcessorSlots = 20;
    EmergencySlots = 2;
    StagedPreparedSlots = 200;
  }

  void Scheduler::SetSlots(int pre_processor, int post_processor, int delivery, int emergency, int staged_prepared) {
    if (scheduler_state == INITIATED) {
      if(pre_processor > 0) PreProcessorSlots = pre_processor;
      if(post_processor > 0) PostProcessorSlots = post_processor;
      if(delivery > 0) DeliverySlots = delivery;
      if(emergency > 0) EmergencySlots = emergency;
      if(staged_prepared > 0) StagedPreparedSlots = staged_prepared;
    }
  }

  void Scheduler::AddURLMapping(const Arc::URL& template_url, const Arc::URL& replacement_url, const Arc::URL& access_url) {
    if (scheduler_state == INITIATED)
      url_map.add(template_url,replacement_url,access_url);
    // else should log warning, but logger is disconnected
  }

  void Scheduler::SetURLMapping(const Arc::URLMap& mapping) {
    if (scheduler_state == INITIATED)
      url_map = mapping;
  }

  void Scheduler::SetPreferredPattern(const std::string& pattern) {
    if (scheduler_state == INITIATED)
      preferred_pattern = pattern;
  }

  void Scheduler::SetTransferSharesConf(const TransferSharesConf& share_conf) {
    if (scheduler_state == INITIATED)
      transferSharesConf = share_conf;
  }

  void Scheduler::SetTransferParameters(const TransferParameters& params) {
    delivery.SetTransferParameters(params);
  }

  void Scheduler::SetDeliveryServices(const std::vector<Arc::URL>& endpoints) {
    if (scheduler_state == INITIATED)
      configured_delivery_services = endpoints;
  }

  void Scheduler::SetDumpLocation(const std::string& location) {
    dumplocation = location;
  }


  bool Scheduler::start(void) {
    if(scheduler_state == RUNNING || scheduler_state == TO_STOP) return false;
    scheduler_state = RUNNING;
    processor.start();
    delivery.start();
    // if no delivery services set, then use local
    if (configured_delivery_services.empty()) {
      std::vector<Arc::URL> services;
      services.push_back(DTR::LOCAL_DELIVERY);
      configured_delivery_services = services;
    }
    Arc::CreateThreadFunction(&main_thread, this);
    return true;
  }

  void Scheduler::log_to_root_logger(Arc::LogLevel level, const std::string& message) {
    Arc::Logger::getRootLogger().addDestinations(root_destinations);
    logger.msg(level, message);
    Arc::Logger::getRootLogger().removeDestinations();
  }

  /* Function to sort the list of the pointers to DTRs 
   * according to the priorities the DTRs have.
   * DTRs with higher priority go first to the beginning,
   * with lower -- to the end
   */
  bool dtr_sort_predicate(DTR* dtr1, DTR* dtr2)
  {
    return dtr1->get_priority() > dtr2->get_priority();
  }

  void Scheduler::next_replica(DTR* request) {
    if (!request->error()) { // bad logic
      request->set_error_status(DTRErrorStatus::INTERNAL_LOGIC_ERROR,
                                DTRErrorStatus::ERROR_UNKNOWN,
                                "Bad logic: next_replica called when there is no error");
      // TODO: how to deal with these internal errors?
      return;
    }

    // Logic of whether to go for next source or destination
    bool source_error(false);

    if (request->get_error_status().GetErrorLocation() == DTRErrorStatus::ERROR_SOURCE)
      source_error = true;
    else if (request->get_error_status().GetErrorLocation() == DTRErrorStatus::ERROR_DESTINATION)
      source_error = false;
    else if (request->get_source()->IsIndex() && !request->get_destination()->IsIndex())
      source_error = true;
    else if (!request->get_source()->IsIndex() && request->get_destination()->IsIndex())
      source_error = false;
    else if (!request->get_source()->LastLocation() && request->get_destination()->LastLocation())
      source_error = true;
    else if (request->get_source()->LastLocation() && !request->get_destination()->LastLocation())
      source_error = false;
    else
      // Unknown error location, and either both are index services with remaining
      // replicas or neither are index services. Choose source in this case.
      source_error = true;

    bool replica_exists;
    if (source_error) {
      // reset mapped file
      request->set_mapped_source();
      replica_exists = request->get_source()->NextLocation();

    } else {
      replica_exists = request->get_destination()->NextLocation();
    }

    if (replica_exists) {
      // Use next replica
      // Clear the error flag to resume normal workflow
      request->reset_error_status();
      request->get_logger()->msg(Arc::INFO, "DTR %s: Using next %s replica", request->get_short_id(), source_error ? "source" : "destination");
      // Perhaps not necessary to query replica again if the error was in the destination
      // but the error could have been caused by a source problem during transfer
      request->set_status(DTRStatus::QUERY_REPLICA);
    }
    else {
      // No replicas - move to appropriate state for the post-processor to do cleanup
      request->get_logger()->msg(Arc::ERROR, "DTR %s: No more %s replicas", request->get_short_id(), source_error ? "source" : "destination");
      if (request->get_destination()->IsIndex()) {
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Will clean up pre-registered destination", request->get_short_id());
        request->set_status(DTRStatus::REGISTER_REPLICA);
      } else if (!request->get_cache_parameters().cache_dirs.empty() &&
                 (request->get_cache_state() == CACHE_ALREADY_PRESENT || request->get_cache_state() == CACHEABLE)) {
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Will release cache locks", request->get_short_id());
        request->set_status(DTRStatus::PROCESS_CACHE);
      } else { // nothing to clean up - set to end state
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Moving to end of data staging", request->get_short_id());
        request->set_status(DTRStatus::CACHE_PROCESSED);
      }
    }
  }

  bool Scheduler::handle_mapped_source(DTR* request, Arc::URL& mapped_url) {
    // The DTR source is mapped to another place so set the mapped location in request.
    // If mapped_url is set delivery will use it as source
    request->get_logger()->msg(Arc::INFO, "DTR %s: Source is mapped to %s", request->get_short_id(), mapped_url.str());

    if (!request->get_source()->ReadOnly() && mapped_url.Protocol() == "link") {
      // read-write access means user can potentially modify source, so copy instead
      request->get_logger()->msg(Arc::WARNING, "DTR %s: Cannot link to source which can be modified, will copy instead");
      mapped_url.ChangeProtocol("file");
    }
    if (mapped_url.Protocol() == "link") {
#ifndef WIN32
      // If the map is a link then do the link here and set to TRANSFERRED. Local file
      // copies should still have to wait in the queue. For links we should also
      // turn off caching, remembering that we still need to release any cache
      // locks later if necessary.
      if (!request->get_destination()->Local()) {
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Cannot link to a remote destination. Will not use mapped URL", request->get_short_id());
      }
      else {
        request->get_logger()->msg(Arc::INFO, "DTR %s: Linking mapped file", request->get_short_id());
        // Access session dir under mapped user
        if (!Arc::FileLink(mapped_url.Path(),
                           request->get_destination()->CurrentLocation().Path(),
                           request->get_local_user().get_uid(),
                           request->get_local_user().get_gid(),
                           true)) {
          request->get_logger()->msg(Arc::ERROR, "DTR %s: Failed to create link: %s. Will not use mapped URL",
                                     request->get_short_id(), Arc::StrError(errno));
        }
        else {
          // successful link, so turn off caching, set to TRANSFERRED and return
          request->set_mapped_source(mapped_url.str());
          if (request->get_cache_state() == CACHEABLE)
            request->set_cache_state(CACHE_NOT_USED);
          request->set_status(DTRStatus::TRANSFERRED);
          return true;
        }
      }
#else
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Linking mapped file - can't link on Windows", request->get_short_id());
#endif
    }
    else {
      // Ready to copy mapped file
      // Assume that mapped urls are not index services or stageable
      // TODO: handle case when mapped url is index 
      request->set_mapped_source(mapped_url.str());
      request->set_status(DTRStatus::STAGED_PREPARED);
      return true;
    }
    return false;
  }

  void Scheduler::ProcessDTRNEW(DTR* request){

    request->get_logger()->msg(Arc::INFO, "Scheduler received new DTR %s with source: %s,"
        " destination: %s, assigned to transfer share %s with priority %d",
        request->get_id(), request->get_source()->str(), request->get_destination()->str(),
        request->get_transfer_share(), request->get_priority());

    // Normal workflow is CHECK_CACHE
    if (request->get_cache_state() == NON_CACHEABLE || request->get_cache_parameters().cache_dirs.empty()) {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: File is not cacheable, was requested not to be cached or no cache available, skipping cache check", request->get_short_id());
      request->set_status(DTRStatus::CACHE_CHECKED);
    } else {
      // Cache checking should have quite a long timeout as it may
      // take a long time to download a big file
      request->set_timeout(3600);
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: File is cacheable, will check cache", request->get_short_id());
      if (DtrList.is_being_cached(request)) {
        Arc::Period cache_wait_period(10);
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: File is currently being cached, will wait %is",
                                   request->get_short_id(), cache_wait_period.GetPeriod());
        request->set_process_time(cache_wait_period);
        request->set_status(DTRStatus::CACHE_WAIT);
      } else {
        request->set_status(DTRStatus::CHECK_CACHE);
      }
    }
  }
  
  void Scheduler::ProcessDTRCACHE_WAIT(DTR* request){
    // The waiting time should be calculated within DTRList so
    // by the time we are here we know to query the cache again

    // If we timed out on it send to CACHE_PROCESSED where it
    // may be retried without caching
    if(request->get_timeout() < time(NULL)) {
      request->set_error_status(DTRErrorStatus::CACHE_ERROR,
                                DTRErrorStatus::ERROR_DESTINATION,
                                "Timed out while waiting for cache for " + request->get_source()->str());
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Timed out while waiting for cache lock", request->get_short_id());
      request->set_status(DTRStatus::CACHE_PROCESSED);
    } else if (DtrList.is_being_cached(request)) {
      // TODO A low priority DTR holding the cache lock can block a high priority DTR
      // downloading the same file. Here we should cancel the low priority one to let
      // the high priority one go through
      Arc::Period cache_wait_period(10);
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: File is currently being cached, will wait %is",
                                 request->get_short_id(), cache_wait_period.GetPeriod());
      request->set_process_time(cache_wait_period);
    } else {
      // Try to check cache again
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Checking cache again", request->get_short_id());
      request->set_status(DTRStatus::CHECK_CACHE);
    }
  }
  
  void Scheduler::ProcessDTRCACHE_CHECKED(DTR* request){
    // There's no need to check additionally for cache error
    // If the error has occurred -- we just proceed the normal
    // workflow as if it was not cached at all.
    // But we should clear error flag if it was set by the pre-processor
    request->reset_error_status();

    if(request->get_cache_state() == CACHE_ALREADY_PRESENT){
      // File is on place already. After the post-processor
      // the DTR is DONE.
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Destination file is in cache", request->get_short_id());
      request->set_status(DTRStatus::PROCESS_CACHE);
    } else if (request->get_source()->IsIndex() || request->get_destination()->IsIndex()) {
      // The Normal workflow -- RESOLVE
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Source and/or destination is index service, will resolve replicas", request->get_short_id());
      request->set_status(DTRStatus::RESOLVE);
    } else {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Neither source nor destination are index services, will skip resolving replicas", request->get_short_id());
      request->set_status(DTRStatus::RESOLVED);
    }
  }
  
  void Scheduler::ProcessDTRRESOLVED(DTR* request){
    if(request->error()){
      // It's impossible to download anything, since no replica location is resolved
      // if cacheable, move to PROCESS_CACHE, the post-processor will do the cleanup
      if (request->get_cache_state() == CACHEABLE && !request->get_cache_parameters().cache_dirs.empty()) {
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Problem with index service, will release cache lock", request->get_short_id());
        request->set_status(DTRStatus::PROCESS_CACHE);
      // else go to end state
      } else {
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Problem with index service, will proceed to end of data staging", request->get_short_id());
        request->set_status(DTRStatus::CACHE_PROCESSED);
      }
    } else {
      // Normal workflow is QUERY_REPLICA
      // Should we always do this?

      // logic to choose best replica - sort according to configured preference
      request->get_source()->SortLocations(preferred_pattern, url_map);
      // Access latency is not known until replica is queried
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Checking source file is present", request->get_short_id());
      request->set_status(DTRStatus::QUERY_REPLICA);
    }
  }
  
  void Scheduler::ProcessDTRREPLICA_QUERIED(DTR* request){
    if(request->error()){
      // go to next replica or exit with error
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Error with source file, moving to next replica", request->get_short_id());
      next_replica(request);
      return;
    }
    // Check if the replica is mapped
    if (url_map) {
      Arc::URL mapped_url(request->get_source()->CurrentLocation());
      if (url_map.map(mapped_url)) {
        if (handle_mapped_source(request, mapped_url))
          return;
      }
    }
    if (request->get_mapped_source().empty() &&
        request->get_source()->GetAccessLatency() == Arc::DataPoint::ACCESS_LATENCY_LARGE) {
      // If the current source location is long latency, try the next replica
      // TODO add this replica to the end of location list, so that if there
      // are problems with other replicas, we eventually come back to this one
      request->get_logger()->msg(Arc::INFO, "DTR %s: Replica %s has long latency, trying next replica", request->get_short_id(), request->get_source()->CurrentLocation().str());
      if (request->get_source()->LastLocation()) {
        request->get_logger()->msg(Arc::INFO, "DTR %s: No more replicas, will use %s", request->get_short_id(), request->get_source()->CurrentLocation().str());
      } else {
        request->get_source()->NextLocation();
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Checking replica %s", request->get_short_id(), request->get_source()->CurrentLocation().str());
        request->set_status(DTRStatus::QUERY_REPLICA);
        return;
      }
    }
    // Normal workflow is PRE_CLEAN state
    // Delete destination if requested in URL options and not replication
    if (!request->is_replication() &&
        (request->get_destination()->GetURL().Option("overwrite") == "yes" ||
         request->get_destination()->CurrentLocation().Option("overwrite") == "yes")) {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Overwrite requested - will pre-clean destination", request->get_short_id());
      request->set_status(DTRStatus::PRE_CLEAN);
    } else {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: No overwrite requested or allowed, skipping pre-cleaning", request->get_short_id());
      request->set_status(DTRStatus::PRE_CLEANED);
    }
  }

  void Scheduler::ProcessDTRPRE_CLEANED(DTR* request){
    // If an error occurred in pre-cleaning, try to copy anyway
    if (request->error())
      request->get_logger()->msg(Arc::INFO, "DTR %s: Pre-clean failed, will still try to copy", request->get_short_id());
    request->reset_error_status();
    if (request->get_source()->IsStageable() || request->get_destination()->IsStageable()) {
      // Normal workflow is STAGE_PREPARE

      // Apply limit to staging to avoid preparing too many files and then
      // pins expire while in the transfer queue. In future it may be better
      // to limit per remote host. For now count staging transfers in this
      // share already in transfer queue and apply limit. In order not to block
      // the highest priority DTRs here we allow them to bypass the limit.
      std::list<DTR*> StagedQueue;
      DtrList.filter_dtrs_by_statuses(DTRStatus::StagedStates, StagedQueue);

      int share_queue = 0, highest_priority = 0;
      for (std::list<DTR*>::iterator dtr = StagedQueue.begin(); dtr != StagedQueue.end(); ++dtr) {
        if ((*dtr)->get_transfer_share() == request->get_transfer_share() &&
            ((*dtr)->get_source()->IsStageable() ||
             (*dtr)->get_destination()->IsStageable())) {
          ++share_queue;
          if ((*dtr)->get_priority() > highest_priority) highest_priority = (*dtr)->get_priority();
        }
      }
      if (share_queue >= StagedPreparedSlots && request->get_priority() <= highest_priority) {
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Large transfer queue - will wait 10s before staging", request->get_short_id());
        request->set_process_time(10);
      }
      else {
        // Need to set the timeout to prevent from waiting for too long
        request->set_timeout(3600);
        // processor will take care of staging source or destination or both
        request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Source or destination requires staging", request->get_short_id());
        request->set_status(DTRStatus::STAGE_PREPARE);
      }
    }
    else {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: No need to stage source or destination, skipping staging", request->get_short_id());
      request->set_status(DTRStatus::STAGED_PREPARED);
    }
  }
  
  void Scheduler::ProcessDTRSTAGING_PREPARING_WAIT(DTR* request){
    // The waiting time should be calculated within DTRList so
    // by the time we are here we know to query the request again

    // If there's timeout -- it's error case
    if(request->get_timeout() < time(NULL)){
      // With a special error status we signal to the post-processor
      // that after releasing request this DTR should go into
      // QUERY_REPLICA again if necessary

      // Here we can't tell at which end the timeout was, so make an educated guess
      if (request->get_source()->IsStageable() && !request->get_destination()->IsStageable())
        request->set_error_status(DTRErrorStatus::STAGING_TIMEOUT_ERROR,
                                  DTRErrorStatus::ERROR_SOURCE,
                                  "Stage request for source file timed out");
      else if (!request->get_source()->IsStageable() && request->get_destination()->IsStageable())
        request->set_error_status(DTRErrorStatus::STAGING_TIMEOUT_ERROR,
                                  DTRErrorStatus::ERROR_DESTINATION,
                                  "Stage request for destination file timed out");
      else // both endpoints are stageable - don't know the error location
        request->set_error_status(DTRErrorStatus::STAGING_TIMEOUT_ERROR,
                                  DTRErrorStatus::ERROR_UNKNOWN,
                                  "Stage request for source or destination file timed out");

      // Let the post-processor do the job
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Staging request timed out, will release request", request->get_short_id());
      request->set_status(DTRStatus::RELEASE_REQUEST);
    } else {
      // Normal workflow is STAGE_PREPARE again
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Querying status of staging request", request->get_short_id());
      request->set_status(DTRStatus::STAGE_PREPARE);
    }
  }
  
  void Scheduler::ProcessDTRSTAGED_PREPARED(DTR* request){
    if(request->error()){
      // We have to try another replica if the source failed to stage
      // but first we have to release any requests
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Releasing requests", request->get_short_id());
      request->set_status(DTRStatus::RELEASE_REQUEST);
      return;
    }
    if (url_map && request->get_mapped_source().empty() && request->get_source()->IsStageable()) {
      // check if any TURLs are mapped
      std::vector<Arc::URL> turls = request->get_source()->TransferLocations();
      for (std::vector<Arc::URL>::iterator i = turls.begin(); i != turls.end(); ++i) {
        Arc::URL mapped_url(i->fullstr());
        if (url_map.map(mapped_url)) {
          if (handle_mapped_source(request, mapped_url))
            return;
        }
      }
    }

    // After normal workflow the DTR is ready for delivery
    request->get_logger()->msg(Arc::VERBOSE, "DTR %s: DTR is ready for transfer, moving to delivery queue", request->get_short_id());

    // set long timeout for waiting for transfer slot
    // (setting timeouts for active transfers is done in Delivery)
    request->set_timeout(7200);
    request->set_status(DTRStatus::TRANSFER);
  }
  
  void Scheduler::ProcessDTRTRANSFERRED(DTR* request){
    // We don't check if error has happened - if it has the post-processor
    // will take needed steps in RELEASE_REQUEST in any case. The error flag
    // will work now as a sign to return the DTR to QUERY_REPLICA again.

    // Delivery will clean up destination physical file on error
    if (request->error())
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Transfer failed: %s", request->get_short_id(), request->get_error_status().GetDesc());

    // Resuming normal workflow after the DTR has finished transferring
    // The next state is RELEASE_REQUEST

    // if cacheable and no cancellation or error, mark the DTR as CACHE_DOWNLOADED
    // Might be better to do this in delivery instead
    if (!request->cancel_requested() && !request->error() && request->get_cache_state() == CACHEABLE)
      request->set_cache_state(CACHE_DOWNLOADED);
    if (request->get_source()->IsStageable() || request->get_destination()->IsStageable()) {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Releasing request(s) made during staging", request->get_short_id());
      request->set_status(DTRStatus::RELEASE_REQUEST);
    } else {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Neither source nor destination were staged, skipping releasing requests", request->get_short_id());
      request->set_status(DTRStatus::REQUEST_RELEASED);
    }
  }
  
  void Scheduler::ProcessDTRREQUEST_RELEASED(DTR* request){
    // if the post-processor had troubles releasing the request, continue
    // normal workflow and the DTR will be cleaned up. If the error
    // originates from before (like Transfer errors, staging errors)
    // and is not from destination, we need to query another replica
    if (request->error() &&
        request->get_error_status().GetLastErrorState() != DTRStatus::RELEASING_REQUEST) {
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Trying next replica", request->get_short_id());
      next_replica(request);
    } else if (request->get_destination()->IsIndex()) {
      // Normal workflow is REGISTER_REPLICA
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Will %s in destination index service", request->get_short_id(),
                                 ((request->error() || request->cancel_requested()) ? "unregister":"register"));
      request->set_status(DTRStatus::REGISTER_REPLICA);
    } else {
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Destination is not index service, skipping replica registration", request->get_short_id());
      request->set_status(DTRStatus::REPLICA_REGISTERED);
    }
  }
  
  void Scheduler::ProcessDTRREPLICA_REGISTERED(DTR* request){
    // If there was a problem registering the destination file,
    // using a different source replica won't help, so pass to final step
    // (remote destinations can't be cached). The post-processor should have
    // taken care of deleting the physical file. If the error originates from
    // before, follow normal workflow and processor will clean up
    if(request->error() &&
       request->get_error_status().GetLastErrorState() == DTRStatus::REGISTERING_REPLICA) {
      request->get_logger()->msg(Arc::ERROR, "DTR %s: Error registering replica, moving to end of data staging", request->get_short_id());
      request->set_status(DTRStatus::CACHE_PROCESSED);
    } else if (!request->get_cache_parameters().cache_dirs.empty() &&
               (request->get_cache_state() == CACHE_ALREADY_PRESENT ||
                request->get_cache_state() == CACHE_DOWNLOADED ||
                request->get_cache_state() == CACHEABLE ||
                request->get_cache_state() == CACHE_NOT_USED)) {
      // Normal workflow is PROCESS_CACHE
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Will process cache", request->get_short_id());
      request->set_status(DTRStatus::PROCESS_CACHE);
    } else {
      // not a cacheable file
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: File is not cacheable, skipping cache processing", request->get_short_id());
      request->set_status(DTRStatus::CACHE_PROCESSED);
    }
  }
  
  void Scheduler::ProcessDTRCACHE_PROCESSED(DTR* request){
    // Final stage within scheduler. Retries are initiated from here if necessary,
    // otherwise report success or failure to generator
    if (request->cancel_requested()) {
      // Cancellation steps finished
      request->get_logger()->msg(Arc::VERBOSE, "DTR %s: Cancellation complete", request->get_short_id());
      request->set_status(DTRStatus::CANCELLED);
    }
    else if(request->error()) {
      // If the error occurred in cache processing we send back
      // to REPLICA_QUERIED to try the same replica again without cache,
      // or to CACHE_CHECKED if the file was already in cache. If there
      // was a cache timeout we also go back to CACHE_CHECKED. If in
      // another place we are finished and report error to generator
      if (request->get_error_status().GetLastErrorState() == DTRStatus::PROCESSING_CACHE) {
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Error in cache processing, will retry without caching", request->get_short_id());
        request->reset_error_status();
        if (request->get_cache_state() == CACHE_ALREADY_PRESENT) request->set_status(DTRStatus::CACHE_CHECKED);
        else request->set_status(DTRStatus::REPLICA_QUERIED);
        request->set_cache_state(CACHE_SKIP);
        return;
      }
      else if (request->get_error_status().GetLastErrorState() == DTRStatus::CACHE_WAIT) {
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Will retry without caching", request->get_short_id());
        request->set_cache_state(CACHE_SKIP);
        request->reset_error_status();
        request->set_status(DTRStatus::CACHE_CHECKED);
        return;
      }
      else {
        request->decrease_tries_left();
        // Here we decide to retry based on whether the error is
        // temporary or not and the configured retry strategy
        if (request->get_error_status().GetErrorStatus() == DTRErrorStatus::TEMPORARY_REMOTE_ERROR ||
            request->get_error_status().GetErrorStatus() == DTRErrorStatus::TRANSFER_SPEED_ERROR ||
            request->get_error_status().GetErrorStatus() == DTRErrorStatus::INTERNAL_PROCESS_ERROR) {
          if (request->get_tries_left() > 0) {
            request->set_process_time(10);
            request->get_logger()->msg(Arc::INFO, "DTR %s: %i retries left, will wait until %s before next attempt",
                                       request->get_short_id(), request->get_tries_left(), request->get_process_time().str());
            // set state depending on where the error occurred
            if (request->get_error_status().GetLastErrorState() == DTRStatus::REGISTERING_REPLICA) {
              request->set_status(DTRStatus::REGISTER_REPLICA);
            } else if (request->get_error_status().GetLastErrorState() == DTRStatus::RELEASING_REQUEST) {
              request->set_status(DTRStatus::RELEASE_REQUEST);
            } else {
              // If error happened before or during transfer set back to NEW
              // Reset DTR information set during this transfer
              request->reset();
              request->set_status(DTRStatus::NEW);
            }
            return;
          }
          else
            request->get_logger()->msg(Arc::ERROR, "DTR %s: Out of retries", request->get_short_id());
        }
        request->get_logger()->msg(Arc::ERROR, "DTR %s: Permanent failure", request->get_short_id());
        request->set_status(DTRStatus::ERROR);
      }
    }
    else {
      // Normal workflow is completed for this DTR successfully
      request->get_logger()->msg(Arc::INFO, "DTR %s: Finished successfully", request->get_short_id());
      request->set_status(DTRStatus::DONE);
    }
  }
  
  void Scheduler::ProcessDTRFINAL_STATE(DTR* request){
  	// This is the only place where the DTR is returned to the generator
  	// and deleted from the global list

  	// Return to the generator
    request->get_logger()->msg(Arc::INFO, "DTR %s: Returning to generator", request->get_short_id());
    request->push(GENERATOR);
    // Delete from the global list
    DtrList.delete_dtr(request);
  }
  
  void Scheduler::map_state_and_process(DTR* request){
    // For cancelled DTRs set the appropriate post-processor state
    if(request->cancel_requested()) map_cancel_state(request);
    // Loop until the DTR is sent somewhere for some action to be done
    // This is more efficient because many DTRs will skip some states and
    // we don't want to have to wait for the full list to be processed before
    // advancing to the next state
    Arc::Time now;
    while((request->came_from_pre_processor() ||
           request->came_from_delivery() ||
           request->came_from_post_processor() ||
           request->came_from_generator()) &&
          request->get_process_time() <= now) {

      switch (request->get_status().GetStatus()) {
        case DTRStatus::NEW: ProcessDTRNEW(request); continue;
        case DTRStatus::CACHE_WAIT: ProcessDTRCACHE_WAIT(request); continue;
        case DTRStatus::CACHE_CHECKED: ProcessDTRCACHE_CHECKED(request); continue;
        case DTRStatus::RESOLVED: ProcessDTRRESOLVED(request); continue;
        case DTRStatus::REPLICA_QUERIED: ProcessDTRREPLICA_QUERIED(request); continue;
        case DTRStatus::PRE_CLEANED: ProcessDTRPRE_CLEANED(request); continue;
        case DTRStatus::STAGING_PREPARING_WAIT: ProcessDTRSTAGING_PREPARING_WAIT(request); continue;
        case DTRStatus::STAGED_PREPARED: ProcessDTRSTAGED_PREPARED(request); continue;
        case DTRStatus::TRANSFERRED: ProcessDTRTRANSFERRED(request); continue;
        case DTRStatus::REQUEST_RELEASED: ProcessDTRREQUEST_RELEASED(request); continue;
        case DTRStatus::REPLICA_REGISTERED: ProcessDTRREPLICA_REGISTERED(request); continue;
        case DTRStatus::CACHE_PROCESSED: ProcessDTRCACHE_PROCESSED(request); continue;
        default: ; //DoNothing
      }
    }

  }
  
  void Scheduler::map_cancel_state(DTR* request){
    switch (request->get_status().GetStatus()) {
      case DTRStatus::NEW:
      case DTRStatus::CHECK_CACHE:
      case DTRStatus::CACHE_WAIT:
        {
          // Nothing has yet been done to require cleanup or additional
          // activities. Return to the generator via CACHE_PROCESSED.
          request->set_status(DTRStatus::CACHE_PROCESSED);
        }
        break;
      case DTRStatus::CACHE_CHECKED:
      case DTRStatus::RESOLVE:
        {
          // The cache may have been started, so set to
          // REPLICA_REIGSTERED to allow post-processor to clean up cache
          request->set_status(DTRStatus::REPLICA_REGISTERED);
        }
        break;
      case DTRStatus::RESOLVED:
      case DTRStatus::QUERY_REPLICA:
      case DTRStatus::REPLICA_QUERIED:
      case DTRStatus::PRE_CLEAN:
      case DTRStatus::PRE_CLEANED:
      case DTRStatus::STAGE_PREPARE:
        {
          // At this stage we may have registered a file in an
          // index service so set to REQUEST_RELEASED to allow
          // the post-processor to clean it up
          request->set_status(DTRStatus::REQUEST_RELEASED);
        }
        break;
      case DTRStatus::STAGING_PREPARING_WAIT:
      case DTRStatus::STAGED_PREPARED:
      case DTRStatus::TRANSFER:
        {
          // At this stage we in addition to cache work
          // may already have pending requests.
          // The post-processor should take care of it too
          request->set_status(DTRStatus::TRANSFERRED);
        }
        break;
      case DTRStatus::TRANSFERRED:
      case DTRStatus::RELEASE_REQUEST:
      case DTRStatus::REQUEST_RELEASED:
      case DTRStatus::REGISTER_REPLICA:
      case DTRStatus::REPLICA_REGISTERED:
      case DTRStatus::PROCESS_CACHE:
      case DTRStatus::CACHE_PROCESSED:
        {
          // post-processing states
          // If the request was cancelled during the transfer, the delivery
          // should have cleaned up the destination file. If after the
          // transfer we have to decide whether to clean up or not.
          /* delete_destination_file() */

          // No other action required here, just let the normal workflow
          // resume and the post-processor will take care of clean up
        }
        break;
      default: ; //Do Nothing
    }
  }
  
  void Scheduler::add_event(DTR* event) {
    event_lock.lock();
    events.push_back(event);
    event_lock.unlock();
  }

  void Scheduler::choose_delivery_service(DTR* request) {
    if (configured_delivery_services.empty()) return;

    // Remember current endpoint
    Arc::URL delivery_endpoint(request->get_delivery_endpoint());

    // When the first DTR is processed, all delivery services are checked here
    // This method assumes that the first DTR has permission on all services,
    // which may not be true if DN filtering is used on those services.
    if (usable_delivery_services.empty()) {
      for (std::vector<Arc::URL>::iterator service = configured_delivery_services.begin();
           service != configured_delivery_services.end(); ++service) {
        request->set_delivery_endpoint(*service);
        std::vector<std::string> allowed_dirs;
        if (!DataDeliveryComm::CheckComm(request, allowed_dirs)) {
          log_to_root_logger(Arc::WARNING, "Error with delivery service at " +
                             request->get_delivery_endpoint().str() + " - This service will not be used");
        }
        else {
          usable_delivery_services[*service] = allowed_dirs;
        }
      }
      request->set_delivery_endpoint(Arc::URL());
      if (usable_delivery_services.empty()) {
        log_to_root_logger(Arc::ERROR, "No usable delivery services found, will use local delivery");
        return;
      }
    }

    // Make a list of the delivery services that this DTR can use
    std::vector<Arc::URL> possible_delivery_services;
    for (std::map<Arc::URL, std::vector<std::string> >::iterator service = usable_delivery_services.begin();
         service != usable_delivery_services.end(); ++service) {
      for (std::vector<std::string>::iterator dir = service->second.begin(); dir != service->second.end(); ++dir) {
        if (request->get_destination()->Local()) {

          // check for caching
          std::string dest = request->get_destination()->TransferLocations()[0].Path();
          if ((request->get_cache_state() == CACHEABLE) && !request->get_cache_file().empty()) dest = request->get_cache_file();

          if (dest.find(*dir) == 0) {
            request->get_logger()->msg(Arc::DEBUG, "DTR %s: Delivery service at %s can copy to %s",
                                       request->get_short_id(), service->first.str(), *dir);
            possible_delivery_services.push_back(service->first);
            break;
          }
        }
        else if (request->get_source()->Local()) {

          if (request->get_source()->TransferLocations()[0].Path().find(*dir) == 0) {
            request->get_logger()->msg(Arc::DEBUG, "DTR %s: Delivery service at %s can copy from %s",
                                       request->get_short_id(), service->first.str(), *dir);
            possible_delivery_services.push_back(service->first);
            break;
          }
        }
        else {
          // copy between two remote endpoints so any service is ok
          possible_delivery_services.push_back(service->first);
          break;
        }
      }
    }
    if (possible_delivery_services.empty()) return;

    // If this is a retry, use a different service
    if (request->get_tries_left() < request->get_initial_tries() && possible_delivery_services.size() > 1) {
      Arc::URL ep(delivery_endpoint);
      // Find a random service different from the previous one, looping a
      // limited number of times in case all delivery services are the same url
      for (unsigned int i = 0; ep == delivery_endpoint && i < possible_delivery_services.size() * 10; ++i) {
        ep = possible_delivery_services.at(rand() % possible_delivery_services.size());
      }
      delivery_endpoint = ep;
    } else {
      delivery_endpoint = possible_delivery_services.at(rand() % possible_delivery_services.size());
    }
    request->set_delivery_endpoint(delivery_endpoint);
  }

  void Scheduler::process_events(void){
    
    Arc::Time now;
    event_lock.lock();

    for (std::list<DTR*>::iterator event = events.begin(); event != events.end();) {
      DTR* tmp = *event;
      event_lock.unlock();

      if (tmp->get_process_time() <= now) {
        map_state_and_process(tmp);
        // If final state, the DTR is returned to the generator and deleted
        if (tmp->is_in_final_state()) {
          ProcessDTRFINAL_STATE(tmp);
          event_lock.lock();
          event = events.erase(event);
          continue;
        }
        // If the event was sent on to a queue, erase it from the list
        if (tmp->is_destined_for_pre_processor() ||
            tmp->is_destined_for_delivery() ||
            tmp->is_destined_for_post_processor()) {
          event_lock.lock();
          event = events.erase(event);
          continue;
        }
      }
      event_lock.lock();
      ++event;
    }
    event_lock.unlock();
  }

  void Scheduler::revise_queues() {

    // The DTRs ready to go into a processing state
    std::map<DTRStatus::DTRStatusType, std::list<DTR*> > DTRQueueStates;
    DtrList.filter_dtrs_by_statuses(DTRStatus::ToProcessStates, DTRQueueStates);

    // The active DTRs currently in processing states
    std::map<DTRStatus::DTRStatusType, std::list<DTR*> > DTRRunningStates;
    DtrList.filter_dtrs_by_statuses(DTRStatus::ProcessingStates, DTRRunningStates);

    // Go through "to process" states, work out shares and push DTRs
    for (unsigned int i = 0; i < DTRStatus::ToProcessStates.size(); ++i) {

      std::list<DTR*> DTRQueue = DTRQueueStates[DTRStatus::ToProcessStates.at(i)];
      std::list<DTR*> ActiveDTRs = DTRRunningStates[DTRStatus::ProcessingStates.at(i)];

      if (DTRQueue.empty() && ActiveDTRs.empty()) continue;

      // Transfer shares for this queue
      TransferShares transferShares(transferSharesConf);

      // Sort the DTR queue according to the priorities the DTRs have.
      // Highest priority will be at the beginning of the list.
      DTRQueue.sort(dtr_sort_predicate);

      int highest_priority = 0;

      // First go over the queue and check for cancellation and timeout
      for (std::list<DTR*>::iterator dtr = DTRQueue.begin(); dtr != DTRQueue.end();) {

        DTR* tmp = *dtr;
        if (dtr == DTRQueue.begin()) highest_priority = tmp->get_priority();

        // There's no check for cancellation requests for the post-processor.
        // Most DTRs with cancellation requests will go to the post-processor
        // for cleanups, hold releases, etc., so the cancellation requests
        // don't break normal workflow in the post-processor (as opposed
        // to any other process), but instead act just as a sign that the
        // post-processor should do additional cleanup activities.
        if (tmp->is_destined_for_pre_processor() || tmp->is_destined_for_delivery()) {

          // The cancellation requests break the normal workflow. A cancelled
          // request will either go back to generator or be put into a
          // post-processor state for clean up.
          if (tmp->cancel_requested()) {
            map_cancel_state(tmp);
            add_event(tmp);
            dtr = DTRQueue.erase(dtr);
            continue;
          }
        }
        // To avoid the situation where DTRs get blocked due to higher
        // priority DTRs, DTRs that have passed their timeout should have their
        // priority boosted. But this should only happen if there are higher
        // priority DTRs, since there could be a large queue of low priority DTRs
        // which, after having their priority boosted, would then block new
        // high priority requests.
        // The simple solution here is to increase priority by 1 every 5 minutes.
        // There is plenty of scope for more intelligent solutions.
        // TODO reset priority back to original value once past this stage.
        if (tmp->get_timeout() < time(NULL) && tmp->get_priority() < highest_priority) {
          tmp->set_priority(tmp->get_priority() + 1);
          tmp->set_timeout(300);
        }
        transferShares.increase_transfer_share(tmp->get_transfer_share());
        ++dtr;
      }

      // Go over the active DTRs and add to transfer share
      for (std::list<DTR*>::iterator dtr = ActiveDTRs.begin(); dtr != ActiveDTRs.end();) {

        DTR* tmp = *dtr;
        // If the DTR is in Delivery, check for cancellation. The pre- and
        // post-processor DTRs don't get cancelled here but are allowed to
        // continue processing.
        if (tmp->get_status() == DTRStatus::TRANSFERRING && tmp->cancel_requested()) {
          tmp->get_logger()->msg(Arc::INFO, "DTR %s: Cancelling active transfer", tmp->get_short_id());
          delivery.cancelDTR(tmp);
          dtr = ActiveDTRs.erase(dtr);
          continue;
        }
        transferShares.increase_transfer_share((*dtr)->get_transfer_share());
        ++dtr;
      }

      // If the queue is empty we can go straight to the next state
      if (DTRQueue.empty()) continue;

      // Slot limit for this state
      int slot_limit = DeliverySlots;
      if (DTRQueue.front()->is_destined_for_pre_processor()) slot_limit = PreProcessorSlots;
      else if (DTRQueue.front()->is_destined_for_post_processor()) slot_limit = PostProcessorSlots;

      // Calculate the slots available for each active share
      transferShares.calculate_shares(slot_limit);

      // Shares which have at least one DTR active and running.
      // Shares can only use emergency slots if they are not in this list.
      std::set<std::string> active_shares;
      int running = ActiveDTRs.size();

      // Go over the active DTRs again and decrease slots in corresponding shares
      for (std::list<DTR*>::iterator dtr = ActiveDTRs.begin(); dtr != ActiveDTRs.end(); ++dtr) {
        transferShares.decrease_number_of_slots((*dtr)->get_transfer_share());
        active_shares.insert((*dtr)->get_transfer_share());
      }

      // Now at the beginning of the queue we have DTRs that should be
      // launched first. Launch them, but with respect to the transfer shares.
      for (std::list<DTR*>::iterator dtr = DTRQueue.begin(); dtr != DTRQueue.end(); ++dtr) {

        DTR* tmp = *dtr;

        // Check if there are any shares left in the queue which might need
        // an emergency share - if not we are done
        if (running >= slot_limit &&
            transferShares.active_shares().size() == active_shares.size()) break;

        // Are there slots left for this share?
        bool can_start = transferShares.can_start(tmp->get_transfer_share());
        // Check if it is possible to use an emergency share
        if (running >= slot_limit &&
            active_shares.find(tmp->get_transfer_share()) != active_shares.end()) {
          can_start = false;
        }

        if (can_start) {
          transferShares.decrease_number_of_slots(tmp->get_transfer_share());

          // Send to processor/delivery
          if (tmp->is_destined_for_pre_processor()) tmp->push(PRE_PROCESSOR);
          else if (tmp->is_destined_for_post_processor()) tmp->push(POST_PROCESSOR);
          else if (tmp->is_destined_for_delivery()) {
            choose_delivery_service(tmp);
            tmp->push(DELIVERY);
          }

          ++running;
          active_shares.insert(tmp->get_transfer_share());
        }
        // Hard limit with all emergency slots used
        if (running == slot_limit + EmergencySlots) break;
      }
    }
  }

  void Scheduler::receiveDTR(DTR& request){

    if (request.get_status() != DTRStatus::NEW) {
      add_event(&request);
      return;
    }
    // New DTR - first check it is valid
    if (!request) {
      logger.msg(Arc::ERROR, "Scheduler received invalid DTR");
      request.set_status(DTRStatus::ERROR);
      request.push(GENERATOR);
      return;
    }

    request.registerCallback(&processor,PRE_PROCESSOR);
    request.registerCallback(&processor,POST_PROCESSOR);
    request.registerCallback(&delivery,DELIVERY);
    /* Shares part*/
    // First, get the transfer share this dtr should belong to
    std::string DtrTransferShare = transferSharesConf.extract_share_info(request);

    // If no share information could be obtained, use default share
    if (DtrTransferShare.empty())
      DtrTransferShare = "_default";

    // If this share is a reference share, we have to add the sub-share
    // to the reference list
    bool in_reference = transferSharesConf.is_configured(DtrTransferShare);
    int priority = transferSharesConf.get_basic_priority(DtrTransferShare);

    request.set_transfer_share(DtrTransferShare);
    DtrTransferShare = request.get_transfer_share();

    // Now the sub-share is added to DtrTransferShare, add it to reference
    // shares if appropriate and update each TransferShare
    if (in_reference && !transferSharesConf.is_configured(DtrTransferShare)) {
      transferSharesConf.set_reference_share(DtrTransferShare, priority);
    }
    
    // Compute the priority this DTR receives - this is the priority of the
    // share adjusted by the priority of the parent job
    request.set_priority(int(transferSharesConf.get_basic_priority(DtrTransferShare) * request.get_priority() * 0.01));
    /* Shares part ends*/               

    DTR * new_dtr = DtrList.add_dtr(request);
    if (new_dtr) {
      add_event(new_dtr);
    }
  }

  bool Scheduler::cancelDTRs(const std::string& jobid) {
    cancelled_jobs_lock.lock();
    cancelled_jobs.push_back(jobid);
    cancelled_jobs_lock.unlock();
    return true;
  }

  void Scheduler::dump_thread(void* arg) {
    Scheduler* sched = (Scheduler*)arg;
    while (sched->scheduler_state == RUNNING && !sched->dumplocation.empty()) {
      // every second, dump state
      sched->DtrList.dumpState(sched->dumplocation);
      if (sched->dump_signal.wait(1000)) break; // notified by signal()
    }
  }

  bool Scheduler::stop() {
    if(scheduler_state != RUNNING) return false;

    // cancel all jobs
    std::list<std::string> alljobs = DtrList.all_jobs();
    cancelled_jobs_lock.lock();
    for (std::list<std::string>::iterator job = alljobs.begin(); job != alljobs.end(); ++job)
      cancelled_jobs.push_back(*job);
    cancelled_jobs_lock.unlock();

    // signal main loop to stop and wait for completion of all DTRs
    scheduler_state = TO_STOP;
    run_signal.wait();
    scheduler_state = STOPPED;
    return true;
  }

  void Scheduler::main_thread (void* arg) {
    Scheduler* it = (Scheduler*)arg;
    it->main_thread();
  }

  void Scheduler::main_thread (void) {
  	
    logger.msg(Arc::INFO, "Scheduler starting up");
    logger.msg(Arc::INFO, "Scheduler configuration:");
    logger.msg(Arc::INFO, "  Pre-processor slots: %i", PreProcessorSlots);
    logger.msg(Arc::INFO, "  Delivery slots: %i", DeliverySlots);
    logger.msg(Arc::INFO, "  Post-processor slots: %i", PostProcessorSlots);
    logger.msg(Arc::INFO, "  Emergency slots: %i", EmergencySlots);
    logger.msg(Arc::INFO, "  Prepared slots: %i", StagedPreparedSlots);
    logger.msg(Arc::INFO, "  Shares configuration:\n%s", transferSharesConf.conf());
    for (std::vector<Arc::URL>::iterator i = configured_delivery_services.begin();
         i != configured_delivery_services.end(); ++i) {
      if (*i == DTR::LOCAL_DELIVERY) logger.msg(Arc::INFO, "  Delivery service: LOCAL");
      else logger.msg(Arc::INFO, "  Delivery service: %s", i->str());
    }

    // Start thread dumping DTR state
    if (!Arc::CreateThreadFunction(&dump_thread, this))
      logger.msg(Arc::ERROR, "Failed to create DTR dump thread");

    // Disconnect from root logger so that messages are logged to per-DTR Logger
    Arc::Logger::getRootLogger().setThreadContext();
    root_destinations = Arc::Logger::getRootLogger().getDestinations();
    Arc::Logger::getRootLogger().removeDestinations();

    while(scheduler_state != TO_STOP || !DtrList.all_dtrs().empty()) {
      // first check for cancelled jobs
      cancelled_jobs_lock.lock();
      std::list<std::string>::iterator jobid = cancelled_jobs.begin();
      for (;jobid != cancelled_jobs.end();) {
        std::list<DTR*> requests;
        DtrList.filter_dtrs_by_job(*jobid, requests);
        for (std::list<DTR*>::iterator dtr = requests.begin(); dtr != requests.end(); ++dtr) {
          (*dtr)->set_cancel_request();
          (*dtr)->get_logger()->msg(Arc::INFO, "DTR %s cancelled", (*dtr)->get_id());
        }
        jobid = cancelled_jobs.erase(jobid);
      }
      cancelled_jobs_lock.unlock();

      // Dealing with pending events, i.e. DTRs from another processes
      process_events();
      // Revise all the internal queues and take actions
      revise_queues();

      Glib::usleep(50000);
    }
    // make sure final state is dumped before exit
    dump_signal.signal();
    if (!dumplocation.empty()) DtrList.dumpState(dumplocation);

    log_to_root_logger(Arc::INFO, "Scheduler loop exited");
    run_signal.signal();
  }
  
} // namespace DataStaging
