#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "DataDeliveryComm.h"
#include "DataDelivery.h"

namespace DataStaging {

  Arc::Logger DataDelivery::logger(Arc::Logger::getRootLogger(), "DataStaging.DataDelivery");

  /// Wrapper class around DataDeliveryComm
  class DataDelivery::delivery_pair_t {
    public:
    DTR* dtr;
    TransferParameters params;
    DataDeliveryComm* comm;
    bool cancelled;
    delivery_pair_t(DTR* request, const TransferParameters& params);
    ~delivery_pair_t();
    void start();
  };

  DataDelivery::delivery_pair_t::delivery_pair_t(DTR* request, const TransferParameters& params)
    :dtr(request),params(params),comm(NULL),cancelled(false) {}

  DataDelivery::delivery_pair_t::~delivery_pair_t() {
    if (comm) delete comm;
  }

  void DataDelivery::delivery_pair_t::start() {
    comm = DataDeliveryComm::CreateInstance(*dtr, params);
  }

  DataDelivery::DataDelivery(): delivery_state(INITIATED) {
  }

  bool DataDelivery::start() {
    if(delivery_state == RUNNING || delivery_state == TO_STOP) return false;
    delivery_state = RUNNING;
    Arc::CreateThreadFunction(&main_thread,this);
    return true;
  }

  void DataDelivery::receiveDTR(DTR& dtr) {
    if(!dtr) {
      logger.msg(Arc::ERROR, "Received invalid DTR");
      dtr.set_error_status(DTRErrorStatus::INTERNAL_LOGIC_ERROR, DTRErrorStatus::ERROR_UNKNOWN, "Invalid DTR");
      dtr.set_status(DTRStatus::TRANSFERRED);
      dtr.push(SCHEDULER);
      return;
    }
    dtr.get_logger()->msg(Arc::INFO, "Delivery received new DTR %s with source: %s, destination: %s",
               dtr.get_id(), dtr.get_source()->CurrentLocation().str(), dtr.get_destination()->CurrentLocation().str());

    dtr.set_status(DTRStatus::TRANSFERRING);
    delivery_pair_t* d = new delivery_pair_t(&dtr, transfer_params);
    dtr_list_lock.lock();
    dtr_list.push_back(d);
    dtr_list_lock.unlock();
    return;
  }

  bool DataDelivery::cancelDTR(DTR* request) {
    if(!request) {
      logger.msg(Arc::ERROR, "Received no DTR");
      return false;
    }
    if(!(*request)) {
      logger.msg(Arc::ERROR, "Received invalid DTR");
      request->set_status(DTRStatus::ERROR);
      return false;
    }
    dtr_list_lock.lock();
    for (std::list<delivery_pair_t*>::iterator i = dtr_list.begin(); i != dtr_list.end(); ++i) {
      delivery_pair_t* ip = *i;
      if (ip->dtr->get_id() == request->get_id()) {
        request->get_logger()->msg(Arc::INFO, "Cancelling DTR %s with source: %s, destination: %s",
                   request->get_id(), request->get_source()->str(), request->get_destination()->str());
        ip->cancelled = true;
        ip->dtr->set_status(DTRStatus::TRANSFERRING_CANCEL);
        dtr_list_lock.unlock();
        return true;
      }
    }
    // DTR is not in the active transfer list, probably because it just finished
    dtr_list_lock.unlock();
    request->get_logger()->msg(Arc::WARNING, "DTR %s requested cancel but no active transfer",
                   request->get_id());
    // if request is already TRANSFERRED, no need to push to Scheduler again
    if (request->get_status() != DTRStatus::TRANSFERRED) {
      request->set_status(DTRStatus::TRANSFERRED);
      request->push(SCHEDULER);
    }
    return true;
  }

  bool DataDelivery::stop() {
    if(delivery_state != RUNNING) return false;
    delivery_state = TO_STOP;
    run_signal.wait();
    delivery_state = STOPPED;
    return true;
  }

  void DataDelivery::SetTransferParameters(const TransferParameters& params) {
    transfer_params = params;
  }

  void DataDelivery::start_delivery(void* arg) {
    delivery_pair_t* dp = (delivery_pair_t*)arg;
    dp->start();
  }

  void DataDelivery::main_thread (void* arg) {
    DataDelivery* it = (DataDelivery*)arg;
    it->main_thread();
  }

  void DataDelivery::main_thread (void) {
    // disconnect from root logger so
    // messages are logged to per-DTR Logger
    Arc::Logger::getRootLogger().setThreadContext();
    Arc::Logger::getRootLogger().removeDestinations();

    while(delivery_state != TO_STOP){
      dtr_list_lock.lock();
      std::list<delivery_pair_t*>::iterator d = dtr_list.begin();
      dtr_list_lock.unlock();
      for(;;) {
        dtr_list_lock.lock();
        if(d == dtr_list.end()) {
          dtr_list_lock.unlock();
          break;
        }
        dtr_list_lock.unlock();
        delivery_pair_t* dp = *d;
        // first check for cancellation
        if (dp->cancelled) {
          dtr_list_lock.lock();
          d = dtr_list.erase(d);
          dtr_list_lock.unlock();

          // deleting delivery_pair_t kills the spawned process
          // Do this before passing back to Scheduler to avoid race condition
          // of DTR being deleted before Comm object has finished with it
          DTR* tmp = dp->dtr;
          delete dp;
          tmp->set_status(DTRStatus::TRANSFERRED);
          tmp->push(SCHEDULER);
          continue;
        }
        // check for new transfer
        if (!dp->comm) {
          // Connecting to a remote delivery service can hang in rare cases,
          // so launch a separate thread with a timeout
          Arc::SimpleCounter thread_count;
          bool res = Arc::CreateThreadFunction(&start_delivery, dp, &thread_count);
          if (res) {
            res = thread_count.wait(300*1000);
          }
          if (!res) {
            // error or timeout - in this case do not delete dp since if the
            // thread timed out it may wake up at some point. Better to have a
            // small memory leak than seg fault.
            dtr_list_lock.lock();
            d = dtr_list.erase(d);
            dtr_list_lock.unlock();

            DTR* tmp = dp->dtr;
            tmp->set_error_status(DTRErrorStatus::INTERNAL_PROCESS_ERROR,
                                      DTRErrorStatus::NO_ERROR_LOCATION,
                                      "Failed to start thread to start delivery or thread timed out");
            tmp->set_status(DTRStatus::TRANSFERRED);
            tmp->push(SCHEDULER);

          } else {
            dtr_list_lock.lock();
            ++d;
            dtr_list_lock.unlock();
          }
          continue;
        }
        // ongoing transfer - get status
        DataDeliveryComm::Status status;
        status = dp->comm->GetStatus();
        dp->dtr->set_bytes_transferred(status.transferred);

        if((status.commstatus == DataDeliveryComm::CommExited) ||
           (status.commstatus == DataDeliveryComm::CommClosed) ||
           (status.commstatus == DataDeliveryComm::CommFailed)) {
          // Transfer finished - either successfully or with error
          // comm.GetError()
          dtr_list_lock.lock();
          d = dtr_list.erase(d);
          dtr_list_lock.unlock();
          if((status.commstatus == DataDeliveryComm::CommFailed) ||
             (status.error != DTRErrorStatus::NONE_ERROR)) {
            if(status.error == DTRErrorStatus::NONE_ERROR)
              status.error = DTRErrorStatus::INTERNAL_PROCESS_ERROR;
            dp->dtr->set_error_status(status.error,status.error_location,
                     status.error_desc[0]?status.error_desc:dp->comm->GetError().c_str());
          } else if (status.checksum) {
            dp->dtr->get_destination()->SetCheckSum(status.checksum);
          }
          dp->dtr->get_logger()->msg(Arc::INFO, "DTR %s: Transfer finished: %llu bytes transferred %s",
                                     dp->dtr->get_short_id(), status.transferred,
                                     (status.checksum[0] ? ": checksum "+std::string(status.checksum) : " "));
          dp->dtr->set_status(DTRStatus::TRANSFERRED);
          dp->dtr->push(SCHEDULER);
          delete dp;
          continue;
        }
        if(!(*(dp->comm))) {
          // Error happened
          // comm.GetError()
          dtr_list_lock.lock();
          d = dtr_list.erase(d);
          dtr_list_lock.unlock();
          dp->dtr->set_error_status(DTRErrorStatus::INTERNAL_PROCESS_ERROR,DTRErrorStatus::ERROR_TRANSFER,
                   dp->comm->GetError().empty()?"Connection with delivery process lost":dp->comm->GetError());
          dp->dtr->set_status(DTRStatus::TRANSFERRED);
          dp->dtr->push(SCHEDULER);
          delete dp;
          continue;
        }
        dtr_list_lock.lock();
        ++d;
        dtr_list_lock.unlock();
      }
      	
      // TODO: replace with condition
      Glib::usleep(500000);
    }
    // Kill any transfers still running
    dtr_list_lock.lock();
    for (std::list<delivery_pair_t*>::iterator d = dtr_list.begin(); d != dtr_list.end();) {
      delete *d;
      d = dtr_list.erase(d);
    }
    dtr_list_lock.unlock();

    logger.msg(Arc::INFO, "Data delivery loop exited");
    run_signal.signal();
  }

  
} // namespace DataStaging
