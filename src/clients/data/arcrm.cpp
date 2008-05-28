#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string>
#include <list>

#include <arc/ArcLocation.h>
#include <arc/Logger.h>
#include <arc/StringConv.h>
#include <arc/URL.h>
#include <arc/data/DataHandle.h>
#include <arc/misc/OptionParser.h>

static Arc::Logger logger(Arc::Logger::getRootLogger(), "arcrm");

void arcrm(const Arc::URL& file_url,
	   bool errcont,
	   int timeout) {
  if (file_url.Protocol() == "filelist") {
    std::list<Arc::URL> files = Arc::ReadURLList(file_url);
    if (files.size() == 0) {
      logger.msg(Arc::ERROR, "Can't read list of locations from file %s",
		 file_url.Path());
      return;
    }
    for (std::list<Arc::URL>::iterator file = files.begin();
	 file != files.end(); file++)
      arcrm(*file, errcont, timeout);
    return;
  }

  Arc::DataHandle url(file_url);
  if (!url) {
    logger.msg(Arc::ERROR, "Unsupported url given");
    return;
  }
  bool remove_lfn = true;
  if (file_url.Locations().size() != 0)
    remove_lfn = false;
  if (!url->Resolve(true))
    if (remove_lfn)
      logger.msg(Arc::INFO,
		 "No locations found - probably no more physical instances");
  /* go through all locations and try to remove files
     physically and their metadata */
  std::list<Arc::URL> removed_urls; // list of physical removed urls
  if (url->HaveLocations())
    for (; url->LocationValid();) {
      logger.msg(Arc::INFO, "Removing %s", url->CurrentLocation().str());
      // It can happen that after resolving list contains duplicated
      // physical locations obtained from different meta-data-services.
      // Because not all locations can reliably say if files does not exist
      // or access is not allowed, avoid duplicated delete attempts.
      bool url_was_deleted = false;
      for (std::list<Arc::URL>::iterator u = removed_urls.begin();
	   u != removed_urls.end(); u++)
	if (url->CurrentLocation() == (*u)) {
	  url_was_deleted = true;
	  break;
	}
      if (url_was_deleted)
	logger.msg(Arc::ERROR, "This instance was already deleted");
      else {
	url->SetSecure(false);
	if (!url->Remove()) {
	  logger.msg(Arc::ERROR, "Failed to delete physical file");
	  if (!errcont) {
	    url->NextLocation();
	    continue;
	  }
	}
	else
	  removed_urls.push_back(url->CurrentLocation());
      }
      if (!url->IsIndex())
	url->RemoveLocation();
      else {
	logger.msg(Arc::INFO, "Removing metadata in %s",
		   url->CurrentLocationMetadata());
	if (!url->Unregister(false)) {
	  logger.msg(Arc::ERROR, "Failed to delete meta-information");
	  url->NextLocation();
	}
      }
    }
  if (url->HaveLocations()) {
    logger.msg(Arc::ERROR, "Failed to remove all physical instances");
    return;
  }
  if (url->IsIndex())
    if (remove_lfn) {
      logger.msg(Arc::ERROR, "Removing logical file from metadata %s",
		 url->str());
      if (!url->Unregister(true)) {
	logger.msg(Arc::ERROR, "Failed to delete logical file");
	return;
      }
    }
  return;
}

int main(int argc, char **argv) {

  setlocale(LC_ALL, "");

  Arc::LogStream logcerr(std::cerr);
  Arc::Logger::getRootLogger().addDestination(logcerr);
  Arc::Logger::getRootLogger().setThreshold(Arc::WARNING);

  Arc::ArcLocation::Init(argv[0]);

  Arc::OptionParser options(istring("url"));

  bool force = false;
  options.AddOption('f', "force",
		    istring("remove logical file name registration even "
			    "if not all physical instances were removed"),
		    force);

  int timeout = 20;
  options.AddOption('t', "timeout", istring("timeout in seconds (default 20)"),
		    istring("seconds"), timeout);

  std::string debug;
  options.AddOption('d', "debug",
		    istring("FATAL, ERROR, WARNING, INFO, DEBUG or VERBOSE"),
		    istring("debuglevel"), debug);

  bool version = false;
  options.AddOption('v', "version", istring("print version information"),
		    version);

  std::list<std::string> params = options.Parse(argc, argv);

  if (!debug.empty())
    Arc::Logger::getRootLogger().setThreshold(Arc::string_to_level(debug));

  if (version) {
    std::cout << Arc::IString("%s version %s", "arcls", VERSION) << std::endl;
    return 0;
  }

  if (params.size() != 1) {
    logger.msg(Arc::ERROR, "Wrong number of parameters specified");
    return 1;
  }

  std::list<std::string>::iterator it = params.begin();
  arcrm(*it, force, timeout);

  return 0;
}
