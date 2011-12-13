// -*- indent-tabs-mode: nil -*-

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string>
#include <list>

#include <arc/ArcLocation.h>
#include <arc/Logger.h>
#include <arc/StringConv.h>
#include <arc/URL.h>
#include <arc/UserConfig.h>
#include <arc/credential/Credential.h>
#include <arc/data/DataHandle.h>
#include <arc/loader/FinderLoader.h>
#include <arc/OptionParser.h>

static Arc::Logger logger(Arc::Logger::getRootLogger(), "arcmkdir");

bool arcmkdir(const Arc::URL& file_url,
              Arc::UserConfig& usercfg,
              bool with_parents) {
  if (!file_url) {
    logger.msg(Arc::ERROR, "Invalid URL: %s", file_url.str());
    return false;
  }
  if (file_url.Protocol() == "filelist") {
    std::list<Arc::URL> files = Arc::ReadURLList(file_url);
    if (files.empty()) {
      logger.msg(Arc::ERROR, "Can't read list of locations from file %s",
                 file_url.Path());
      return false;
    }
    bool r = true;
    for (std::list<Arc::URL>::iterator file = files.begin();
         file != files.end(); file++) {
      if (!arcmkdir(*file, usercfg, with_parents))
        r = false;
    }
    return r;
  }

  if (file_url.IsSecureProtocol()) {
    usercfg.InitializeCredentials();
    if (!Arc::Credential::IsCredentialsValid(usercfg)) {
      logger.msg(Arc::ERROR, "Unable to create directory %s: No valid credentials found", file_url.str());
      return false;
    }
  }

  Arc::DataHandle url(file_url, usercfg);
  if (!url) {
    logger.msg(Arc::ERROR, "Unsupported URL given");
    return false;
  }
  url->SetSecure(false);
  Arc::DataStatus res = url->CreateDirectory(with_parents);
  if (!res.Passed()) {
    logger.msg(Arc::ERROR, "%s%s", std::string(res), (res.GetDesc().empty() ? " " : ": "+res.GetDesc()));
    if (res.Retryable())
      logger.msg(Arc::ERROR, "This seems like a temporary error, please try again later");
    return false;
  }
  return true;
}

int main(int argc, char **argv) {

  setlocale(LC_ALL, "");

  Arc::LogStream logcerr(std::cerr);
  logcerr.setFormat(Arc::ShortFormat);
  Arc::Logger::getRootLogger().addDestination(logcerr);
  Arc::Logger::getRootLogger().setThreshold(Arc::WARNING);

  Arc::ArcLocation::Init(argv[0]);

  Arc::OptionParser options(istring("url"),
                            istring("The arcmkdir command creates directories "
                                    "on grid storage elements and catalogs."));

  bool with_parents = false;
  options.AddOption('p', "parents",
                    istring("make parent directories as needed"),
                    with_parents);

  bool show_plugins = false;
  options.AddOption('P', "listplugins",
                    istring("list the available plugins (protocols supported)"),
                    show_plugins);

  int timeout = 20;
  options.AddOption('t', "timeout", istring("timeout in seconds (default 20)"),
                    istring("seconds"), timeout);

  std::string conffile;
  options.AddOption('z', "conffile",
                    istring("configuration file (default ~/.arc/client.conf)"),
                    istring("filename"), conffile);

  std::string debug;
  options.AddOption('d', "debug",
                    istring("FATAL, ERROR, WARNING, INFO, VERBOSE or DEBUG"),
                    istring("debuglevel"), debug);

  bool version = false;
  options.AddOption('v', "version", istring("print version information"),
                    version);

  std::list<std::string> params = options.Parse(argc, argv);

  if (version) {
    std::cout << Arc::IString("%s version %s", "arcrm", VERSION) << std::endl;
    return 0;
  }

  // If debug is specified as argument, it should be set before loading the configuration.
  if (!debug.empty())
    Arc::Logger::getRootLogger().setThreshold(Arc::string_to_level(debug));

  if (show_plugins) {
    std::list<Arc::ModuleDesc> modules;
    Arc::PluginsFactory pf(Arc::BaseConfig().MakeConfig(Arc::Config()).Parent());
    pf.scan(Arc::FinderLoader::GetLibrariesList(), modules);
    Arc::PluginsFactory::FilterByKind("HED:DMC", modules);

    std::cout << Arc::IString("Protocol plugins available:") << std::endl;
    for (std::list<Arc::ModuleDesc>::iterator itMod = modules.begin();
         itMod != modules.end(); itMod++) {
      for (std::list<Arc::PluginDesc>::iterator itPlug = itMod->plugins.begin();
           itPlug != itMod->plugins.end(); itPlug++) {
        std::cout << "  " << itPlug->name << " - " << itPlug->description << std::endl;
      }
    }
    return 0;
  }

  // credentials will be initialised later if necessary
  Arc::UserConfig usercfg(conffile, Arc::initializeCredentialsType::SkipCredentials);
  if (!usercfg) {
    logger.msg(Arc::ERROR, "Failed configuration initialization");
    return 1;
  }
  usercfg.UtilsDirPath(Arc::UserConfig::ARCUSERDIRECTORY);
  usercfg.Timeout(timeout);

  if (debug.empty() && !usercfg.Verbosity().empty())
    Arc::Logger::getRootLogger().setThreshold(Arc::string_to_level(usercfg.Verbosity()));

  if (params.size() != 1) {
    logger.msg(Arc::ERROR, "Wrong number of parameters specified");
    return 1;
  }

  // add a slash to the end if not present
  std::string url = params.front();
  if (url[url.length()-1] != '/') url += '/';

  if (!arcmkdir(url, usercfg, with_parents))
    return 1;

  return 0;
}