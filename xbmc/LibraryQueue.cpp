/*
 *      Copyright (C) 2014 Team XBMC
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

#include "LibraryQueue.h"
#include "GUIUserMessages.h"
#include "Util.h"
#include "guilib/GUIWindowManager.h"
#include "threads/SingleLock.h"
#include "video/jobs/VideoLibraryCleaningJob.h"
#include "video/jobs/VideoLibraryJob.h"
#include "video/jobs/VideoLibraryMarkWatchedJob.h"
#include "video/jobs/VideoLibraryScanningJob.h"

using namespace std;

CLibraryQueue::CLibraryQueue()
  : CJobQueue(false, 1, CJob::PRIORITY_LOW),
    m_jobs(),
    m_callbacks(),
    m_cleaning(false)
{ }

CLibraryQueue::~CLibraryQueue()
{
  CSingleLock lock(m_critical);
  m_jobs.clear();
  m_callbacks.clear();
}

CLibraryQueue& CLibraryQueue::Get()
{
  static CLibraryQueue s_instance;
  return s_instance;
}

void CLibraryQueue::ScanVideoLibrary(const std::string& directory, bool scanAll /* = false */ , bool showProgress /* = true */)
{
  AddJob(new CVideoLibraryScanningJob(directory, scanAll, showProgress));
}

bool CLibraryQueue::IsScanningLibrary() const
{
  // check if the library is being cleaned synchronously
  if (m_cleaning)
    return true;

  // check if the library is being scanned asynchronously
  LibraryJobMap::const_iterator scanningJobs = m_jobs.find("VideoLibraryScanningJob");
  if (scanningJobs != m_jobs.end() && !scanningJobs->second.empty())
    return true;

  // check if the library is being cleaned asynchronously
  LibraryJobMap::const_iterator cleaningJobs = m_jobs.find("VideoLibraryCleaningJob");
  if (cleaningJobs != m_jobs.end() && !cleaningJobs->second.empty())
    return true;

  return false;
}

void CLibraryQueue::StopLibraryScanning()
{
  CSingleLock lock(m_critical);
  LibraryJobMap::const_iterator scanningJobs = m_jobs.find("VideoLibraryScanningJob");
  if (scanningJobs == m_jobs.end())
    return;

  // get a copy of the scanning jobs because CancelJob() will modify m_scanningJobs
  LibraryJobs tmpScanningJobs(scanningJobs->second.begin(), scanningJobs->second.end());

  // cancel all scanning jobs
  for (LibraryJobs::const_iterator job = tmpScanningJobs.begin(); job != tmpScanningJobs.end(); ++job)
    CancelJob(*job);
  Refresh();
}

void CLibraryQueue::CleanVideoLibrary(const std::set<int>& paths /* = std::set<int>() */, bool asynchronous /* = true */, CGUIDialogProgressBarHandle* progressBar /* = NULL */)
{
  CVideoLibraryCleaningJob* cleaningJob = new CVideoLibraryCleaningJob(paths, progressBar);

  if (asynchronous)
    AddJob(cleaningJob);
  else
  {
    m_cleaning = true;
    cleaningJob->DoWork();

    delete cleaningJob;
    m_cleaning = false;
    Refresh();
  }
}

void CLibraryQueue::CleanVideoLibraryModal(const std::set<int>& paths /* = std::set<int>() */)
{
  // we can't perform a modal library cleaning if other jobs are running
  if (IsRunning())
    return;

  m_cleaning = true;
  CVideoLibraryCleaningJob cleaningJob(paths, true);
  cleaningJob.DoWork();
  m_cleaning = false;
  Refresh();
}

void CLibraryQueue::MarkAsWatched(const CFileItemPtr &item, bool watched)
{
  if (item == NULL)
    return;

  AddJob(new CVideoLibraryMarkWatchedJob(item, watched));
}

void CLibraryQueue::AddJob(CLibraryJob* job, IJobCallback* callback /* = NULL */)
{
  if (job == NULL)
    return;

  CSingleLock lock(m_critical);
  if (!CJobQueue::AddJob(job))
    return;

  // add the job to our list of queued/running jobs
  std::string jobType = job->GetType();
  LibraryJobMap::iterator jobsIt = m_jobs.find(jobType);
  if (jobsIt == m_jobs.end())
  {
    LibraryJobs jobs;
    jobs.insert(job);
    m_jobs.insert(std::make_pair(jobType, jobs));
  }
  else
    jobsIt->second.insert(job);

  // if there's a specific callback, add it to the callback map
  if (callback != NULL)
    m_callbacks.insert(std::make_pair(job, callback));
}

void CLibraryQueue::CancelJob(CLibraryJob* job)
{
  if (job == NULL)
    return;

  CSingleLock lock(m_critical);
  // remember the job type needed later because the job might be deleted
  // in the call to CJobQueue::CancelJob()
  std::string jobType;
  if (job->GetType() != NULL)
    jobType = job->GetType();

  // check if the job supports cancellation and cancel it
  if (job->CanBeCancelled())
    job->Cancel();

  // remove the job from the job queue
  CJobQueue::CancelJob(job);

  // remove the job from our list of queued/running jobs
  LibraryJobMap::iterator jobsIt = m_jobs.find(jobType);
  if (jobsIt != m_jobs.end())
    jobsIt->second.erase(job);

  // remove the job (and its callback) from the callback map
  m_callbacks.erase(job);
}

void CLibraryQueue::CancelAllJobs()
{
  CSingleLock lock(m_critical);
  CJobQueue::CancelJobs();

  // remove all jobs
  m_jobs.clear();
  m_callbacks.clear();
}

bool CLibraryQueue::IsRunning() const
{
  return CJobQueue::IsProcessing() || m_cleaning;
}

void CLibraryQueue::Refresh()
{
  CUtil::DeleteVideoDatabaseDirectoryCache();
  CGUIMessage msg(GUI_MSG_NOTIFY_ALL, 0, 0, GUI_MSG_UPDATE);
  g_windowManager.SendThreadMessage(msg);
}

void CLibraryQueue::OnJobProgress(unsigned int jobID, unsigned int progress, unsigned int total, const CJob* job)
{
  if (job == NULL)
    return;

  // check if we need to call a specific callback
  LibraryJobCallbacks::iterator callback = m_callbacks.find(static_cast<const CLibraryJob*>(job));
  if (callback != m_callbacks.end())
    callback->second->OnJobProgress(jobID, progress, total, job);

  // let the generic job queue do its work
  CJobQueue::OnJobProgress(jobID, progress, total, job);
}

void CLibraryQueue::OnJobComplete(unsigned int jobID, bool success, CJob* job)
{
  if (success)
  {
    if (QueueEmpty())
      Refresh();
  }

  {
    CSingleLock lock(m_critical);
    CLibraryJob* libraryJob = static_cast<CLibraryJob*>(job);

    // check if we need to call a specific callback
    LibraryJobCallbacks::iterator callback = m_callbacks.find(libraryJob);
    if (callback != m_callbacks.end())
      callback->second->OnJobComplete(jobID, success, job);

    // remove the job from our list of queued/running jobs
    LibraryJobMap::iterator jobsIt = m_jobs.find(job->GetType());
    if (jobsIt != m_jobs.end())
      jobsIt->second.erase(libraryJob);
  }

  // let the generic job queue do its work
  return CJobQueue::OnJobComplete(jobID, success, job);
}