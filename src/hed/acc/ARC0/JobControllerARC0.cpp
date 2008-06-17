#include "JobControllerARC0.h"
#include <arc/XMLNode.h>
#include <arc/ArcConfig.h>
#include <arc/data/DataBufferPar.h>
#include <arc/data/DataHandle.h>
#include <arc/StringConv.h>

#include <map>
#include <iostream>
#include <algorithm>

namespace Arc {

  JobControllerARC0::JobControllerARC0(Arc::Config *cfg) : JobController(cfg, "ARC0") {}
  
  JobControllerARC0::~JobControllerARC0() {}

  ACC *JobControllerARC0::Instance(Config *cfg, ChainContext *) {
    return new JobControllerARC0(cfg);
  }
  
  void JobControllerARC0::GetJobInformation(){

    //1. Sort jobs according to host and base dn (path)
    std::list<Job>::iterator iter;
    std::map< std::string, std::list<Job*> > SortedByHost; 

    for(iter=JobStore.begin();iter != JobStore.end(); iter++){
      SortedByHost[iter->InfoEndpoint.Host()+"/"+iter->InfoEndpoint.Path()].push_back(&*iter);  
    }
    
    //Actions below this point could(should) be threaded in the future

    std::map< std::string, std::list<Job*> >::iterator HostIterator;

    for(HostIterator = SortedByHost.begin(); HostIterator != SortedByHost.end(); HostIterator++){
      //2. Copy info endpoint url from one of the jobs      
      URL FinalURL = (*HostIterator->second.begin())->InfoEndpoint;
      //3a. Prepare new filter      
      std::string filter = "(|";
      std::list<Job*>::iterator it;      
      for(it = HostIterator->second.begin(); it != HostIterator->second.end(); it++){
	filter += (*it)->InfoEndpoint.LDAPFilter();
      }
      filter += ")";
      //3b. Change filter of the final url      
      FinalURL.ChangeLDAPFilter(filter);
      
      std::cout<<"Final URL = " << FinalURL.str() << std::endl;

      //4. Read from information source
      
      DataHandle handler(FinalURL);
      DataBufferPar buffer;

      if (!handler->StartReading(buffer))
	return;

      int handle;
      unsigned int length;
      unsigned long long int offset;
      std::string result;

      while (buffer.for_write() || !buffer.eof_read())
	if (buffer.for_write(handle, length, offset, true)) {
	  result.append(buffer[handle], length);
	  buffer.is_written(handle);
	}

      if (!handler->StopReading())
	return;

      //5. Fill jobs with information
      XMLNode XMLresult(result);

      std::cout<<"Read job information:"<<std::endl;
      std::cout<<result<<std::endl;
      //XMLresult.SaveToStream(std::cout);
      
      for(it = HostIterator->second.begin(); it != HostIterator->second.end(); it++){
	XMLNode JobXMLInfo = (*XMLresult.XPathLookup("//nordugrid-job-globalid"
						     "[nordugrid-job-globalid='"+(*it)->JobID.str()+"']", NS()).begin());
	if(JobXMLInfo["nordugrid-job-status"])
	  (*it)->State = (std::string) JobXMLInfo["nordugrid-job-status"];
	if(JobXMLInfo["nordugrid-job-globalowner"])
	  (*it)->Owner = (std::string) JobXMLInfo["nordugrid-job-globalowner"];
	if(JobXMLInfo["nordugrid-job-execcluster"])
	  (*it)->ExecutionCE = (std::string) JobXMLInfo["nordugrid-job-execcluster"];
	if(JobXMLInfo["nordugrid-job-execqueue"])
	  (*it)->Queue = (std::string) JobXMLInfo["nordugrid-job-execqueue"];
	if(JobXMLInfo["nordugrid-job-submissionui"])
	  (*it)->SubmissionHost = (std::string) JobXMLInfo["nordugrid-job-submissionui"];
	if(JobXMLInfo["nordugrid-job-submissiontime"])
	  (*it)->SubmissionTime = (std::string) JobXMLInfo["nordugrid-job-submissiontime"];
	if(JobXMLInfo["nordugrid-job-sessiondirerasetime"])
	  (*it)->WorkingAreaEraseTime = (std::string) JobXMLInfo["nordugrid-job-sessiondirerasetime"];
	if(JobXMLInfo["nordugrid-job-proxyexpirationtime"])
	  (*it)->ProxyExpirationTime = (std::string) JobXMLInfo["nordugrid-job-proxyexpirationtime"];
	if(JobXMLInfo["nordugrid-job-completiontime"])
	  (*it)->EndTime = (std::string) JobXMLInfo["nordugrid-job-completiontime"];
	if(JobXMLInfo["nordugrid-job-cpucount"])
	  (*it)->UsedSlots = stringtoi(JobXMLInfo["nordugrid-job-cpucount"]);
	if(JobXMLInfo["nordugrid-job-usedcputime"])
	  (*it)->UsedTotalCPUTime = (std::string) JobXMLInfo["nordugrid-job-usedcputime"];
	if(JobXMLInfo["nordugrid-job-usedwalltime"])
	  (*it)->UsedWallTime = (std::string) JobXMLInfo["nordugrid-job-usedwalltime"];
	if(JobXMLInfo["nordugrid-job-exitcode"])
	  (*it)->ExitCode = stringtoi(JobXMLInfo["nordugrid-job-exitcode"]);
	if(JobXMLInfo["Mds-validfrom"]){
	  (*it)->CreationTime = (std::string) (JobXMLInfo["Mds-validfrom"]);
	  if(JobXMLInfo["Mds-validto"]){
	    Time Validto = (std::string) (JobXMLInfo["Mds-validto"]);
	    (*it)->Validity = Validto - (*it)->CreationTime;
	  }
	}
	if(JobXMLInfo["nordugrid-job-stdout"])
	  (*it)->StdOut = (std::string) (JobXMLInfo["nordugrid-job-stdout"]);
	if(JobXMLInfo["nordugrid-job-stderr"])
	  (*it)->StdErr = (std::string) (JobXMLInfo["nordugrid-job-stderr"]);
	if(JobXMLInfo["nordugrid-job-stdin"])
	  (*it)->StdIn = (std::string) (JobXMLInfo["nordugrid-job-stdin"]);
	if(JobXMLInfo["nordugrid-job-reqcputime"])
	  (*it)->RequestedTotalCPUTime = (std::string) (JobXMLInfo["nordugrid-job-reqcputime"]);
	if(JobXMLInfo["nordugrid-job-reqwalltime"])
	  (*it)->RequestedWallTime = (std::string) (JobXMLInfo["nordugrid-job-reqwalltime"]);
	if(JobXMLInfo["nordugrid-job-rerunable"])
	  (*it)->RestartState = (std::string) (JobXMLInfo["nordugrid-job-rerunable"]);
	if(JobXMLInfo["nordugrid-job-queuerank"])
	  (*it)->WaitingPosition = stringtoi(JobXMLInfo["nordugrid-job-queuerank"]);
	if(JobXMLInfo["nordugrid-job-comment"])
	  (*it)->OtherMessages = (std::string) (JobXMLInfo["nordugrid-job-comment"]);
	if(JobXMLInfo["nordugrid-job-usedmem"])
	  (*it)->UsedMainMemory = stringtoi(JobXMLInfo["nordugrid-job-usedmem"]);
	if(JobXMLInfo["nordugrid-job-errors"]){
	  for(XMLNode n = JobXMLInfo["nordugrid-job-errors"]; n; ++n){
	    (*it)->Error.push_back((std::string) n);
	  }	
	}
	if(JobXMLInfo["nordugrid-job-jobname"])
	  (*it)->Name = (std::string) (JobXMLInfo["nordugrid-job-jobname"]);
	if(JobXMLInfo["nordugrid-job-gmlog"])
	  (*it)->LogDir = (std::string) (JobXMLInfo["nordugrid-job-gmlog"]);
	if(JobXMLInfo["nordugrid-job-clientsofware"])
	  (*it)->SubmissionClientName = (std::string) (JobXMLInfo["nordugrid-job-clientsoftware"]);
	if(JobXMLInfo["nordugrid-job-executionnodes"]){
	  for(XMLNode n = JobXMLInfo["nordugrid-job-executionnodes"]; n; ++n){
	    (*it)->ExecutionNode.push_back((std::string) n);
	  }
	}
	if(JobXMLInfo["nordugrid-job-runtimeenvironment"]){
	  for(XMLNode n = JobXMLInfo["nordugrid-job-runtimeenvironment"]; n; ++n){
	    (*it)->UsedApplicationEnvironment.push_back((std::string) n);
	  }
	}

      }

    } //end loop over the different hosts

  } //end GetJobInformation

  void JobControllerARC0::DownloadJobOutput(){
    
    std::cout<<"Downloading ARC0 jobs"<<std::endl;
    
    //Thread these
    for(std::list<Arc::Job>::iterator jobiter = JobStore.begin(); jobiter!= JobStore.end(); jobiter++){
      DownloadThisJob(*jobiter);
    }
  }
  
  void JobControllerARC0::PerformAction(std::string /* action */){

  }

  void JobControllerARC0::DownloadThisJob(Job ThisJob){
    //1. Find the list of files to be downloaded 
    //   (This part may be grid flavour specific)
    
    std::cout<<"Initializing DataHandle with url: " <<ThisJob.JobID.str()<<std::endl;

    Arc::DataHandle source(ThisJob.JobID);
    
    if(source){
      std::list<FileInfo> outputfiles;
      source->ListFiles(outputfiles, true);
      std::cout<<"Job directory contains:"<<std::endl;
      for(std::list<Arc::FileInfo>::iterator i = outputfiles.begin();i != outputfiles.end(); i++){
	std::cout << i->GetName() <<std::endl;
	std::string src = ThisJob.JobID.str() +"/"+ i->GetName();
	std::string path_temp = ThisJob.JobID.Path(); 
	size_t slash = path_temp.find_first_of("/");
	std::string dst = path_temp.substr(slash+1) + "/" + i->GetName();
	CopyFile(src, dst);
      }
    }

    //2. Copy the file (use JobController base class implementation)
    

  }

} // namespace Arc
