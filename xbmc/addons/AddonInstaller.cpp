/*
 *      Copyright (C) 2011-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AddonInstaller.h"
#include "events/EventLog.h"
#include "events/AddonManagementEvent.h"
#include "events/NotificationEvent.h"
#include "utils/log.h"
#include "utils/FileUtils.h"
#include "utils/URIUtils.h"
#include "utils/Variant.h"
#include "Util.h"
#include "guilib/LocalizeStrings.h"
#include "filesystem/Directory.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "messaging/ApplicationMessenger.h"
#include "filesystem/FavouritesDirectory.h"
#include "utils/JobManager.h"
#include "dialogs/GUIDialogYesNo.h"
#include "addons/AddonManager.h"
#include "addons/Repository.h"
#include "guilib/GUIWindowManager.h"      // for callback
#include "GUIUserMessages.h"              // for callback
#include "utils/StringUtils.h"
#include "dialogs/GUIDialogOK.h"
#include "dialogs/GUIDialogExtendedProgressBar.h"
#include "URL.h"

#include <functional>

using namespace std;
using namespace XFILE;
using namespace ADDON;
using namespace KODI::MESSAGING;

struct find_map : public binary_function<CAddonInstaller::JobMap::value_type, unsigned int, bool>
{
  bool operator() (CAddonInstaller::JobMap::value_type t, unsigned int id) const
  {
    return (t.second.jobID == id);
  }
};

CAddonInstaller::CAddonInstaller()
  : m_repoUpdateJob(nullptr)
{ }

CAddonInstaller::~CAddonInstaller()
{ }

CAddonInstaller &CAddonInstaller::Get()
{
  static CAddonInstaller addonInstaller;
  return addonInstaller;
}

void CAddonInstaller::OnJobComplete(unsigned int jobID, bool success, CJob* job)
{
  if (success)
    CAddonMgr::Get().FindAddons();

  CSingleLock lock(m_critSection);
  if (strncmp(job->GetType(), "repoupdate", 10) == 0)
  {
    // repo job finished
    m_repoUpdateDone.Set();
    m_repoUpdateJob = nullptr;
    lock.Leave();
  }
  else
  {
    // download job
    JobMap::iterator i = find_if(m_downloadJobs.begin(), m_downloadJobs.end(), bind2nd(find_map(), jobID));
    if (i != m_downloadJobs.end())
      m_downloadJobs.erase(i);
    lock.Leave();
    PrunePackageCache();
  }

  CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE);
  g_windowManager.SendThreadMessage(msg);
}

void CAddonInstaller::OnJobProgress(unsigned int jobID, unsigned int progress, unsigned int total, const CJob *job)
{
  CSingleLock lock(m_critSection);
  JobMap::iterator i = find_if(m_downloadJobs.begin(), m_downloadJobs.end(), bind2nd(find_map(), jobID));
  if (i != m_downloadJobs.end())
  {
    // update job progress
    i->second.progress = progress;
    CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE_ITEM);
    msg.SetStringParam(i->first);
    lock.Leave();
    g_windowManager.SendThreadMessage(msg);
  }
}

bool CAddonInstaller::IsDownloading() const
{
  CSingleLock lock(m_critSection);
  return !m_downloadJobs.empty();
}

void CAddonInstaller::GetInstallList(VECADDONS &addons) const
{
  CSingleLock lock(m_critSection);
  vector<std::string> addonIDs;
  for (JobMap::const_iterator i = m_downloadJobs.begin(); i != m_downloadJobs.end(); ++i)
  {
    if (i->second.jobID)
      addonIDs.push_back(i->first);
  }
  lock.Leave();

  CAddonDatabase database;
  database.Open();
  for (vector<std::string>::iterator it = addonIDs.begin(); it != addonIDs.end(); ++it)
  {
    AddonPtr addon;
    if (database.GetAddon(*it, addon))
      addons.push_back(addon);
  }
}

bool CAddonInstaller::GetProgress(const std::string &addonID, unsigned int &percent) const
{
  CSingleLock lock(m_critSection);
  JobMap::const_iterator i = m_downloadJobs.find(addonID);
  if (i != m_downloadJobs.end())
  {
    percent = i->second.progress;
    return true;
  }
  return false;
}

bool CAddonInstaller::Cancel(const std::string &addonID)
{
  CSingleLock lock(m_critSection);
  JobMap::iterator i = m_downloadJobs.find(addonID);
  if (i != m_downloadJobs.end())
  {
    CJobManager::GetInstance().CancelJob(i->second.jobID);
    m_downloadJobs.erase(i);
    return true;
  }

  return false;
}

bool CAddonInstaller::InstallModal(const std::string &addonID, ADDON::AddonPtr &addon, bool promptForInstall /* = true */)
{
  if (!g_passwordManager.CheckMenuLock(WINDOW_ADDON_BROWSER))
    return false;

  // we assume that addons that are enabled don't get to this routine (i.e. that GetAddon() has been called)
  if (CAddonMgr::Get().GetAddon(addonID, addon, ADDON_UNKNOWN, false))
    return false; // addon is installed but disabled, and the user has specifically activated something that needs
                  // the addon - should we enable it?

  // check we have it available
  CAddonDatabase database;
  database.Open();
  if (!database.GetAddon(addonID, addon))
    return false;

  // if specified ask the user if he wants it installed
  if (promptForInstall &&
      !CGUIDialogYesNo::ShowAndGetInput(CVariant{24076}, CVariant{24100}, CVariant{addon->Name()}, CVariant{24101}))
    return false;

  if (!Install(addonID, true, "", false, true))
    return false;

  return CAddonMgr::Get().GetAddon(addonID, addon);
}

bool CAddonInstaller::Install(const std::string &addonID, bool force /* = false */, const std::string &referer /* = "" */, bool background /* = true */, bool modal /* = false */)
{
  AddonPtr addon;
  bool addonInstalled = CAddonMgr::Get().GetAddon(addonID, addon, ADDON_UNKNOWN, false);
  if (addonInstalled && !force)
    return true;

  if (referer.empty())
  {
    if (!g_passwordManager.CheckMenuLock(WINDOW_ADDON_BROWSER))
      return false;
  }

  // check whether we have it available in a repository
  std::string hash;
  if (!CAddonInstallJob::GetAddonWithHash(addonID, addon, hash))
    return false;

  return DoInstall(addon, hash, addonInstalled, referer, background, modal);
}

bool CAddonInstaller::DoInstall(const AddonPtr &addon, const std::string &hash /* = "" */, bool update /* = false */, const std::string &referer /* = "" */, bool background /* = true */, bool modal /* = false */)
{
  // check whether we already have the addon installing
  CSingleLock lock(m_critSection);
  if (m_downloadJobs.find(addon->ID()) != m_downloadJobs.end())
    return false;

  CAddonInstallJob* installJob = new CAddonInstallJob(addon, hash, update, referer);
  if (background)
  {
    unsigned int jobID = CJobManager::GetInstance().AddJob(installJob, this);
    m_downloadJobs.insert(make_pair(addon->ID(), CDownloadJob(jobID)));
    return true;
  }

  m_downloadJobs.insert(make_pair(addon->ID(), CDownloadJob(0)));
  lock.Leave();

  bool result = false;
  if (modal)
    result = installJob->DoModal();
  else
    result = installJob->DoWork();
  delete installJob;

  lock.Enter();
  JobMap::iterator i = m_downloadJobs.find(addon->ID());
  m_downloadJobs.erase(i);

  return result;
}

bool CAddonInstaller::InstallFromZip(const std::string &path)
{
  if (!g_passwordManager.CheckMenuLock(WINDOW_ADDON_BROWSER))
    return false;

  // grab the descriptive XML document from the zip, and read it in
  CFileItemList items;
  // BUG: some zip files return a single item (root folder) that we think is stored, so we don't use the zip:// protocol
  CURL pathToUrl(path);
  CURL zipDir = URIUtils::CreateArchivePath("zip", pathToUrl, "");
  if (!CDirectory::GetDirectory(zipDir, items) || items.Size() != 1 || !items[0]->m_bIsFolder)
  {
    CEventLog::GetInstance().AddWithNotification(
      EventPtr(new CNotificationEvent(EventLevelError, 24045,
                   StringUtils::Format(g_localizeStrings.Get(24143).c_str(), path.c_str()))), false);
    return false;
  }

  // TODO: possibly add support for github generated zips here?
  std::string archive = URIUtils::AddFileToFolder(items[0]->GetPath(), "addon.xml");

  CXBMCTinyXML xml;
  AddonPtr addon;
  if (xml.LoadFile(archive) && CAddonMgr::Get().LoadAddonDescriptionFromMemory(xml.RootElement(), addon))
  {
    // set the correct path
    addon->Props().path = items[0]->GetPath();
    addon->Props().icon = URIUtils::AddFileToFolder(items[0]->GetPath(), "icon.png");

    // install the addon
    return DoInstall(addon);
  }

  CEventLog::GetInstance().AddWithNotification(
    EventPtr(new CNotificationEvent(EventLevelError, 24045,
                 StringUtils::Format(g_localizeStrings.Get(24143).c_str(), path.c_str()))), false);
  return false;
}

bool CAddonInstaller::CheckDependencies(const AddonPtr &addon, CAddonDatabase *database /* = NULL */)
{
  std::pair<std::string, std::string> failedDep;
  return CheckDependencies(addon, failedDep, database);
}

bool CAddonInstaller::CheckDependencies(const AddonPtr &addon, std::pair<std::string, std::string> &failedDep, CAddonDatabase *database /* = NULL */)
{
  std::vector<std::string> preDeps;
  preDeps.push_back(addon->ID());
  CAddonDatabase localDB;
  if (!database)
    database = &localDB;

  return CheckDependencies(addon, preDeps, *database, failedDep);
}

bool CAddonInstaller::CheckDependencies(const AddonPtr &addon,
                                        std::vector<std::string>& preDeps, CAddonDatabase &database,
                                        std::pair<std::string, std::string> &failedDep)
{
  if (addon == NULL)
    return true; // a NULL addon has no dependencies

  if (!database.Open())
    return false;

  ADDONDEPS deps = addon->GetDeps();
  for (ADDONDEPS::const_iterator i = deps.begin(); i != deps.end(); ++i)
  {
    const std::string &addonID = i->first;
    const AddonVersion &version = i->second.first;
    bool optional = i->second.second;
    AddonPtr dep;
    bool haveAddon = CAddonMgr::Get().GetAddon(addonID, dep);
    // we have it but our version isn't good enough, or we don't have it and we need it
    if ((haveAddon && !dep->MeetsVersion(version)) || (!haveAddon && !optional))
    {
      // we don't have it in a repo, or we have it but the version isn't good enough, so dep isn't satisfied.
      CLog::Log(LOGDEBUG, "CAddonInstallJob[%s]: requires %s version %s which is not available", addon->ID().c_str(), addonID.c_str(), version.asString().c_str());
      database.Close();

      // fill in the details of the failed dependency
      failedDep.first = addon->ID();
      failedDep.second = version.asString();

      return false;
    }

    // at this point we have our dep, or the dep is optional (and we don't have it) so check that it's OK as well
    // TODO: should we assume that installed deps are OK?
    if (dep && std::find(preDeps.begin(), preDeps.end(), dep->ID()) == preDeps.end())
    {
      if (!CheckDependencies(dep, preDeps, database, failedDep))
      {
        database.Close();
        preDeps.push_back(dep->ID());
        return false;
      }
    }
  }
  database.Close();

  return true;
}

CDateTime CAddonInstaller::LastRepoUpdate() const
{
  CDateTime update;
  CAddonDatabase database;
  if (!database.Open())
    return update;

  VECADDONS addons;
  CAddonMgr::Get().GetAddons(ADDON_REPOSITORY, addons);
  for (unsigned int i = 0; i < addons.size(); i++)
  {
    CDateTime lastUpdate = database.GetRepoTimestamp(addons[i]->ID());
    if (lastUpdate.IsValid() && lastUpdate > update)
      update = lastUpdate;
  }

  return update;
}

void CAddonInstaller::UpdateRepos(bool force /*= false*/, bool wait /*= false*/, bool showProgress /*= false*/)
{
  CSingleLock lock(m_critSection);
  if (m_repoUpdateJob != nullptr)
  {
    //Hook up dialog to running job
    if (showProgress && !m_repoUpdateJob->HasProgressIndicator())
    {
      auto* dialog = static_cast<CGUIDialogExtendedProgressBar*>(g_windowManager.GetWindow(WINDOW_DIALOG_EXT_PROGRESS));
      if (dialog)
        m_repoUpdateJob->SetProgressIndicators(dialog->GetHandle(g_localizeStrings.Get(24092)), nullptr);
    }

    if (wait)
    {
      // wait for our job to complete
      lock.Leave();
      CLog::Log(LOGDEBUG, "%s - waiting for repository update job to finish...", __FUNCTION__);
      m_repoUpdateDone.Wait();
    }
    return;
  }

  // don't run repo update jobs while on the login screen which runs under the master profile
  if(!force && (g_windowManager.GetActiveWindow() & WINDOW_ID_MASK) == WINDOW_LOGIN_SCREEN)
    return;

  if (!force && m_repoUpdateWatch.IsRunning() && m_repoUpdateWatch.GetElapsedSeconds() < 600)
    return;

  CAddonDatabase database;
  if (!database.Open())
    return;

  m_repoUpdateWatch.StartZero();

  VECADDONS addons;
  if (CAddonMgr::Get().GetAddons(ADDON_REPOSITORY, addons))
  {
    for (const auto& repo : addons)
    {
      CDateTime lastUpdate = database.GetRepoTimestamp(repo->ID());
      if (force || !lastUpdate.IsValid() || lastUpdate + CDateTimeSpan(0, 24, 0, 0) < CDateTime::GetCurrentDateTime()
        || repo->Version() != database.GetRepoVersion(repo->ID()))
      {
        CLog::Log(LOGDEBUG, "Checking repositories for updates (triggered by %s)", repo->Name().c_str());

        m_repoUpdateJob = new CRepositoryUpdateJob(addons);
        if (showProgress)
        {
          auto* dialog = static_cast<CGUIDialogExtendedProgressBar*>(g_windowManager.GetWindow(WINDOW_DIALOG_EXT_PROGRESS));
          if (dialog)
            m_repoUpdateJob->SetProgressIndicators(dialog->GetHandle(g_localizeStrings.Get(24092)), nullptr);
        }
        CJobManager::GetInstance().AddJob(m_repoUpdateJob, this);
        if (wait)
        {
          // wait for our job to complete
          lock.Leave();
          CLog::Log(LOGDEBUG, "%s - waiting for this repository update job to finish...", __FUNCTION__);
          m_repoUpdateDone.Wait();
        }

        return;
      }
    }
  }
}

bool CAddonInstaller::HasJob(const std::string& ID) const
{
  CSingleLock lock(m_critSection);
  return m_downloadJobs.find(ID) != m_downloadJobs.end();
}

void CAddonInstaller::PrunePackageCache()
{
  std::map<std::string,CFileItemList*> packs;
  int64_t size = EnumeratePackageFolder(packs);
  int64_t limit = (int64_t)g_advancedSettings.m_addonPackageFolderSize * 1024 * 1024;
  if (size < limit)
    return;

  // Prune packages
  // 1. Remove the largest packages, leaving at least 2 for each add-on
  CFileItemList items;
  CAddonDatabase db;
  db.Open();
  for (std::map<std::string,CFileItemList*>::const_iterator it = packs.begin(); it != packs.end(); ++it)
  {
    it->second->Sort(SortByLabel, SortOrderDescending);
    for (int j = 2; j < it->second->Size(); j++)
      items.Add(CFileItemPtr(new CFileItem(*it->second->Get(j))));
  }

  items.Sort(SortBySize, SortOrderDescending);
  int i = 0;
  while (size > limit && i < items.Size())
  {
    size -= items[i]->m_dwSize;
    db.RemovePackage(items[i]->GetPath());
    CFileUtils::DeleteItem(items[i++], true);
  }

  if (size > limit)
  {
    // 2. Remove the oldest packages (leaving least 1 for each add-on)
    items.Clear();
    for (std::map<std::string,CFileItemList*>::iterator it = packs.begin(); it != packs.end(); ++it)
    {
      if (it->second->Size() > 1)
        items.Add(CFileItemPtr(new CFileItem(*it->second->Get(1))));
    }

    items.Sort(SortByDate, SortOrderAscending);
    i = 0;
    while (size > limit && i < items.Size())
    {
      size -= items[i]->m_dwSize;
      db.RemovePackage(items[i]->GetPath());
      CFileUtils::DeleteItem(items[i++],true);
    }
  }

  // clean up our mess
  for (std::map<std::string,CFileItemList*>::iterator it = packs.begin(); it != packs.end(); ++it)
    delete it->second;
}

int64_t CAddonInstaller::EnumeratePackageFolder(std::map<std::string,CFileItemList*>& result)
{
  CFileItemList items;
  CDirectory::GetDirectory("special://home/addons/packages/",items,".zip",DIR_FLAG_NO_FILE_DIRS);
  int64_t size = 0;
  for (int i = 0; i < items.Size(); i++)
  {
    if (items[i]->m_bIsFolder)
      continue;

    size += items[i]->m_dwSize;
    std::string pack,dummy;
    AddonVersion::SplitFileName(pack, dummy, items[i]->GetLabel());
    if (result.find(pack) == result.end())
      result[pack] = new CFileItemList;
    result[pack]->Add(CFileItemPtr(new CFileItem(*items[i])));
  }

  return size;
}

CAddonInstallJob::CAddonInstallJob(const AddonPtr &addon, const std::string &hash /* = "" */, bool update /* = false */, const std::string &referer /* = "" */)
  : m_addon(addon),
    m_hash(hash),
    m_update(update),
    m_referer(referer)
{ }

AddonPtr CAddonInstallJob::GetRepoForAddon(const AddonPtr& addon)
{
  AddonPtr repoPtr;

  CAddonDatabase database;
  if (!database.Open())
    return repoPtr;

  std::string repo;
  if (!database.GetRepoForAddon(addon->ID(), repo))
    return repoPtr;

  if (!CAddonMgr::Get().GetAddon(repo, repoPtr))
    return repoPtr;

  if (std::dynamic_pointer_cast<CRepository>(repoPtr) == NULL)
  {
    repoPtr.reset();
    return repoPtr;
  }

  return repoPtr;
}

bool CAddonInstallJob::GetAddonWithHash(const std::string& addonID, ADDON::AddonPtr& addon, std::string& hash)
{
  CAddonDatabase database;
  if (!database.Open())
    return false;

  if (!database.GetAddon(addonID, addon))
    return false;

  AddonPtr ptr = GetRepoForAddon(addon);
  if (ptr == NULL)
    return false;

  RepositoryPtr repo = std::dynamic_pointer_cast<CRepository>(ptr);
  if (repo == NULL)
    return false;

  hash = repo->GetAddonHash(addon);
  return true;
}

bool CAddonInstallJob::DoWork()
{
  SetTitle(StringUtils::Format(g_localizeStrings.Get(24057).c_str(), m_addon->Name().c_str()));
  SetProgress(0);

  // check whether all the dependencies are available or not
  SetText(g_localizeStrings.Get(24058));
  std::pair<std::string, std::string> failedDep;
  if (!CAddonInstaller::Get().CheckDependencies(m_addon, failedDep))
  {
    std::string details = StringUtils::Format(g_localizeStrings.Get(24142).c_str(), failedDep.first.c_str(), failedDep.second.c_str());
    CLog::Log(LOGERROR, "CAddonInstallJob[%s]: %s", m_addon->ID().c_str(), details.c_str());
    ReportInstallError(m_addon->ID(), m_addon->ID(), details);
    return false;
  }

  AddonPtr repoPtr = GetRepoForAddon(m_addon);
  std::string installFrom;
  if (!repoPtr || repoPtr->Props().libname.empty())
  {
    // Addons are installed by downloading the .zip package on the server to the local
    // packages folder, then extracting from the local .zip package into the addons folder
    // Both these functions are achieved by "copying" using the vfs.

    std::string dest = "special://home/addons/packages/";
    std::string package = URIUtils::AddFileToFolder("special://home/addons/packages/",
                                                    URIUtils::GetFileName(m_addon->Path()));
    if (URIUtils::HasSlashAtEnd(m_addon->Path()))
    { // passed in a folder - all we need do is copy it across
      installFrom = m_addon->Path();
    }
    else
    {
      CAddonDatabase db;
      if (!db.Open())
      {
        CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to open database", m_addon->ID().c_str());
        ReportInstallError(m_addon->ID(), m_addon->ID());
        return false;
      }

      std::string md5;
      // check that we don't already have a valid copy
      if (!m_hash.empty() && CFile::Exists(package))
      {
        if (db.GetPackageHash(m_addon->ID(), package, md5) && m_hash != md5)
        {
          db.RemovePackage(package);
          CFile::Delete(package);
        }
      }

      // zip passed in - download + extract
      if (!CFile::Exists(package))
      {
        std::string path(m_addon->Path());
        if (!m_referer.empty() && URIUtils::IsInternetStream(path))
        {
          CURL url(path);
          url.SetProtocolOptions(m_referer);
          path = url.Get();
        }

        if (!DownloadPackage(path, dest))
        {
          CFile::Delete(package);

          CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to download %s", m_addon->ID().c_str(), package.c_str());
          ReportInstallError(m_addon->ID(), URIUtils::GetFileName(package));
          return false;
        }
      }

      // at this point we have the package - check that it is valid
      SetText(g_localizeStrings.Get(24077));
      if (!m_hash.empty())
      {
        md5 = CUtil::GetFileMD5(package);
        if (!StringUtils::EqualsNoCase(md5, m_hash))
        {
          CFile::Delete(package);

          CLog::Log(LOGERROR, "CAddonInstallJob[%s]: MD5 mismatch after download %s", m_addon->ID().c_str(), package.c_str());
          ReportInstallError(m_addon->ID(), URIUtils::GetFileName(package));
          return false;
        }

        db.AddPackage(m_addon->ID(), package, md5);
      }

      // check the archive as well - should have just a single folder in the root
      CURL archive = URIUtils::CreateArchivePath("zip", CURL(package), "");

      CFileItemList archivedFiles;
      CDirectory::GetDirectory(archive, archivedFiles);

      if (archivedFiles.Size() != 1 || !archivedFiles[0]->m_bIsFolder)
      {
        // invalid package
        db.RemovePackage(package);
        CFile::Delete(package);

        CLog::Log(LOGERROR, "CAddonInstallJob[%s]: invalid package %s", m_addon->ID().c_str(), package.c_str());
        ReportInstallError(m_addon->ID(), URIUtils::GetFileName(package));
        return false;
      }

      installFrom = archivedFiles[0]->GetPath();
    }
    repoPtr.reset();
  }

  // run any pre-install functions
  ADDON::OnPreInstall(m_addon);

  // perform install
  if (!Install(installFrom, repoPtr))
    return false;

  // run any post-install guff
  CEventLog::GetInstance().Add(
    EventPtr(new CAddonManagementEvent(m_addon, m_update ? 24065 : 24064)),
    !IsModal() && CSettings::Get().GetBool(CSettings::SETTING_GENERAL_ADDONNOTIFICATIONS), false);

  ADDON::OnPostInstall(m_addon, m_update, IsModal());

  //Clear addon from the disabled table
  CAddonDatabase database;
  database.Open();
  database.DisableAddon(m_addon->ID(), false);

  // and we're done!
  MarkFinished();
  return true;
}

bool CAddonInstallJob::DownloadPackage(const std::string &path, const std::string &dest)
{
  if (ShouldCancel(0, 1))
    return false;

  SetText(g_localizeStrings.Get(24078));

  // need to download/copy the package first
  CFileItemList list;
  list.Add(CFileItemPtr(new CFileItem(path, false)));
  list[0]->Select(true);

  return DoFileOperation(CFileOperationJob::ActionReplace, list, dest, true);
}

bool CAddonInstallJob::DoFileOperation(FileAction action, CFileItemList &items, const std::string &file, bool useSameJob /* = true */)
{
  bool result = false;
  if (useSameJob)
  {
    SetFileOperation(action, items, file);

    // temporarily disable auto-closing so not to close the current progress indicator
    bool autoClose = GetAutoClose();
    if (autoClose)
      SetAutoClose(false);
    // temporarily disable updating title or text
    bool updateInformation = GetUpdateInformation();
    if (updateInformation)
      SetUpdateInformation(false);

    result = CFileOperationJob::DoWork();

    SetUpdateInformation(updateInformation);
    SetAutoClose(autoClose);
  }
  else
  {
   CFileOperationJob job(action, items, file);

   // pass our progress indicators to the temporary job and only allow it to
   // show progress updates (no title or text changes)
   job.SetProgressIndicators(GetProgressBar(), GetProgressDialog(), GetUpdateProgress(), false);

   result = job.DoWork();
  }

  return result;
}

bool CAddonInstallJob::DeleteAddon(const std::string &addonFolder)
{
  CFileItemList list;
  list.Add(CFileItemPtr(new CFileItem(addonFolder, true)));
  list[0]->Select(true);

  return DoFileOperation(CFileOperationJob::ActionDelete, list, "", false);
}

bool CAddonInstallJob::Install(const std::string &installFrom, const AddonPtr& repo)
{
  SetText(g_localizeStrings.Get(24079));
  ADDONDEPS deps = m_addon->GetDeps();

  unsigned int totalSteps = static_cast<unsigned int>(deps.size());
  if (ShouldCancel(0, totalSteps))
    return false;

  // The first thing we do is install dependencies
  std::string referer = StringUtils::Format("Referer=%s-%s.zip",m_addon->ID().c_str(),m_addon->Version().asString().c_str());
  for (ADDONDEPS::iterator it = deps.begin(); it != deps.end(); ++it)
  {
    if (it->first != "xbmc.metadata")
    {
      const std::string &addonID = it->first;
      const AddonVersion &version = it->second.first;
      bool optional = it->second.second;
      AddonPtr dependency;
      bool haveAddon = CAddonMgr::Get().GetAddon(addonID, dependency);
      if ((haveAddon && !dependency->MeetsVersion(version)) || (!haveAddon && !optional))
      {
        // we have it but our version isn't good enough, or we don't have it and we need it
        bool force = dependency != NULL;

        // dependency is already queued up for install - ::Install will fail
        // instead we wait until the Job has finished. note that we
        // recall install on purpose in case prior installation failed
        if (CAddonInstaller::Get().HasJob(addonID))
        {
          while (CAddonInstaller::Get().HasJob(addonID))
            Sleep(50);
          force = false;

          if (!CAddonMgr::Get().IsAddonInstalled(addonID))
          {
            CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to install dependency %s", m_addon->ID().c_str(), addonID.c_str());
            ReportInstallError(m_addon->ID(), m_addon->ID(), g_localizeStrings.Get(24085));
            return false;
          }
        }
        // don't have the addon or the addon isn't new enough - grab it (no new job for these)
        else if (IsModal())
        {
          AddonPtr addon;
          std::string hash;
          if (!CAddonInstallJob::GetAddonWithHash(addonID, addon, hash))
          {
            CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to find dependency %s", m_addon->ID().c_str(), addonID.c_str());
            ReportInstallError(m_addon->ID(), m_addon->ID(), g_localizeStrings.Get(24085));
            return false;
          }

          CAddonInstallJob dependencyJob(addon, hash, force, referer);

          // pass our progress indicators to the temporary job and don't allow it to
          // show progress or information updates (no progress, title or text changes)
          dependencyJob.SetProgressIndicators(GetProgressBar(), GetProgressDialog(), false, false);

          if (!dependencyJob.DoModal())
          {
            CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to install dependency %s", m_addon->ID().c_str(), addonID.c_str());
            ReportInstallError(m_addon->ID(), m_addon->ID(), g_localizeStrings.Get(24085));
            return false;
          }
        }
        else if (!CAddonInstaller::Get().Install(addonID, force, referer, false))
        {
          CLog::Log(LOGERROR, "CAddonInstallJob[%s]: failed to install dependency %s", m_addon->ID().c_str(), addonID.c_str());
          ReportInstallError(m_addon->ID(), m_addon->ID(), g_localizeStrings.Get(24085));
          return false;
        }
      }
    }

    if (ShouldCancel(std::distance(deps.begin(), it), totalSteps))
      return false;
  }

  SetText(g_localizeStrings.Get(24086));
  SetProgress(0);

  // now that we have all our dependencies, we can install our add-on
  if (repo != NULL)
  {
    CFileItemList dummy;
    std::string s = StringUtils::Format("plugin://%s/?action=install&package=%s&version=%s", repo->ID().c_str(),
                                        m_addon->ID().c_str(), m_addon->Version().asString().c_str());
    if (!CDirectory::GetDirectory(s, dummy))
    {
      CLog::Log(LOGERROR, "CAddonInstallJob[%s]: installation of repository failed", m_addon->ID().c_str());
      ReportInstallError(m_addon->ID(), m_addon->ID());
      return false;
    }
  }
  else
  {
    std::string addonFolder = installFrom;
    URIUtils::RemoveSlashAtEnd(addonFolder);
    addonFolder = URIUtils::AddFileToFolder("special://home/addons/", URIUtils::GetFileName(addonFolder));

    CFileItemList install;
    install.Add(CFileItemPtr(new CFileItem(installFrom, true)));
    install[0]->Select(true);

    AddonPtr addon;
    if (!DoFileOperation(CFileOperationJob::ActionReplace, install, "special://home/addons/", false) ||
        !CAddonMgr::Get().LoadAddonDescription(addonFolder, addon))
    {
      // failed extraction or failed to load addon description
      DeleteAddon(addonFolder);

      std::string addonID = URIUtils::GetFileName(addonFolder);
      CLog::Log(LOGERROR, "CAddonInstallJob[%s]: could not read addon description of %s", addonID.c_str(), addonFolder.c_str());
      ReportInstallError(addonID, addonID);
      return false;
    }

    // Update the addon manager so that it has the newly installed add-on.
    CAddonMgr::Get().FindAddons();
  }
  SetProgress(100);

  return true;
}

void CAddonInstallJob::ReportInstallError(const std::string& addonID, const std::string& fileName, const std::string& message /* = "" */)
{
  AddonPtr addon;
  CAddonDatabase database;
  if (database.Open())
  {
    database.GetAddon(addonID, addon);
    database.Close();
  }

  MarkFinished();

  std::string msg = message;
  EventPtr activity;
  if (addon != NULL)
  {
    AddonPtr addon2;
    CAddonMgr::Get().GetAddon(addonID, addon2);
    if (msg.empty())
      msg = g_localizeStrings.Get(addon2 != NULL ? 113 : 114);

    activity = EventPtr(new CAddonManagementEvent(addon, EventLevelError, msg));
    if (IsModal())
      CGUIDialogOK::ShowAndGetInput(CVariant{m_addon->Name()}, CVariant{msg});
  }
  else
  {
    activity =
      EventPtr(new CNotificationEvent(EventLevelError, 24045,
                   !msg.empty() ? msg : StringUtils::Format(g_localizeStrings.Get(24143).c_str(),
                   fileName.c_str())));

    if (IsModal())
      CGUIDialogOK::ShowAndGetInput(CVariant{fileName}, CVariant{msg});
  }

  CEventLog::GetInstance().Add(activity, !IsModal(), false);
}

std::string CAddonInstallJob::AddonID() const
{
  return m_addon ? m_addon->ID() : "";
}

CAddonUnInstallJob::CAddonUnInstallJob(const AddonPtr &addon)
  : m_addon(addon)
{ }

bool CAddonUnInstallJob::DoWork()
{
  ADDON::OnPreUnInstall(m_addon);

  AddonPtr repoPtr = CAddonInstallJob::GetRepoForAddon(m_addon);
  RepositoryPtr therepo = std::dynamic_pointer_cast<CRepository>(repoPtr);
  if (therepo != NULL && !therepo->Props().libname.empty())
  {
    CFileItemList dummy;
    std::string s = StringUtils::Format("plugin://%s/?action=uninstall&package=%s", therepo->ID().c_str(), m_addon->ID().c_str());
    if (!CDirectory::GetDirectory(s, dummy))
      return false;
  }
  else
  {
    //Unregister addon with the manager to ensure nothing tries
    //to interact with it while we are uninstalling.
    CAddonMgr::Get().UnregisterAddon(m_addon->ID());

    if (!DeleteAddon(m_addon->Path()))
    {
      CLog::Log(LOGERROR, "CAddonUnInstallJob[%s]: could not delete addon data.", m_addon->ID().c_str());
      return false;
    }
  }

  ClearFavourites();

  AddonPtr addon;
  CAddonDatabase database;
  // try to get the addon object from the repository as the local one does not exist anymore
  // if that doesn't work fall back to the local one
  if (!database.Open() || !database.GetAddon(m_addon->ID(), addon) || addon == NULL)
    addon = m_addon;
  CEventLog::GetInstance().Add(EventPtr(new CAddonManagementEvent(addon, 24144)));

  ADDON::OnPostUnInstall(m_addon);
  return true;
}

bool CAddonUnInstallJob::DeleteAddon(const std::string &addonFolder)
{
  CFileItemList list;
  list.Add(CFileItemPtr(new CFileItem(addonFolder, true)));
  list[0]->Select(true);

  SetFileOperation(CFileOperationJob::ActionDelete, list, "");
  return CFileOperationJob::DoWork();
}

void CAddonUnInstallJob::ClearFavourites()
{
  bool bSave = false;
  CFileItemList items;
  XFILE::CFavouritesDirectory::Load(items);
  for (int i = 0; i < items.Size(); i++)
  {
    if (items[i]->GetPath().find(m_addon->ID()) != std::string::npos)
    {
      items.Remove(items[i].get());
      bSave = true;
    }
  }

  if (bSave)
    CFavouritesDirectory::Save(items);
}
