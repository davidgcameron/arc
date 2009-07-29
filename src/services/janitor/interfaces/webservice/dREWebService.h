/**      
 * Implementation of a simple echo service
 *
 * The reply of the echo service contains the string which
 * was send to it.
 */

#ifndef __DRE_SERVICE_H__
#define __DRE_SERVICE_H__

#if HAVE_CONFIG_H
	#include <config.h>
#endif


#include <arc/message/Service.h>
#include <arc/Logger.h>


#include "PerlProcessor.h"


namespace DREService
{

class DREWebService: public Arc::Service
{
 	public:


		/**
		* Constructor which is capable to extract prefix and suffix
		* for the echo service.
		*/
        	DREWebService(Arc::Config *cfg);

		/**
		* Destructor.
		*/
	        virtual ~DREWebService(void);

		/**
		* Implementation of the virtual method defined in MCCInterface
		* (to be found in MCC.h). 
		* @param inmsg incoming message
		* @param inmsg outgoing message
		* @return Status of the result achieved
		*/
		virtual Arc::MCC_Status process(Arc::Message& inmsg,Arc::Message& outmsg);






	protected:

		/**
		*  Arc-intern logger. Generates output into the file specified 
		*  in the arched configuration file used to invoke arched services.
		*/
		static Arc::Logger logger;

		/**
		* Class which specifies a XML namespace i.e. "echo".
		* Needed to extract the content out of the incoming message
		*/
		Arc::NS ns_;



		/**
		* Method to return an error. 
		* Creates a fault message and returns a status.
		* @param outmsg outgoing message
		* @return Status of the result achieved
		*/
		Arc::MCC_Status makeFault(Arc::Message& outmsg, const std::string &reason);


		TaskQueue::TaskQueue*           pTaskqueue;
		TaskSet::TaskSet*               pTaskset;
		PerlProcessor::PerlProcessor*   pPerlProcessor;
}; 

} //namespace ArcService

#endif 
