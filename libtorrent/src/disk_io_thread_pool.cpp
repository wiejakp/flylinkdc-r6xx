/*

Copyright (c) 2016-2018, 2020, 2022, Alden Torres
Copyright (c) 2016, 2018, Steven Siloti
Copyright (c) 2017-2020, 2022, Arvid Norberg
All rights reserved.

You may use, distribute and modify this code under the terms of the BSD license,
see LICENSE file.
*/

#include "libtorrent/aux_/disk_io_thread_pool.hpp"
#include "libtorrent/assert.hpp"

#ifndef _DEBUG
#include "libtorrent/aux_/escape_string.hpp" // for convert_to_wstring
// #include "../doctor-dump/CrashRpt.h"
#endif

#include <algorithm>

namespace {

	constexpr std::chrono::seconds reap_idle_threads_interval(60);
}

namespace libtorrent::aux {

	disk_io_thread_pool::disk_io_thread_pool(disk_thread_fun thread_fun
		, io_context& ios)
		: m_thread_fun(std::move(thread_fun))
		, m_max_threads(0)
		, m_threads_to_exit(0)
		, m_abort(false)
		, m_num_idle_threads(0)
		, m_min_idle_threads(0)
		, m_idle_timer(ios)
		, m_ioc(ios)
	{}

	disk_io_thread_pool::~disk_io_thread_pool()
	{
        try // https://github.com/arvidn/libtorrent/issues/1176
        {
            abort(true);
        }
        catch (const std::exception& e) // TODO  catch (const concurrency::scheduler_resource_allocation_error& e)
        {
			m_error_code = e.what();
#ifndef _DEBUG
			//extern crash_rpt::CrashRpt g_crashRpt;
			//g_crashRpt.AddUserInfoToReport(L"T1", libtorrent::convert_to_wstring(m_error_code).c_str());
#endif
			throw;
        }

		TORRENT_ASSERT(num_threads() == 0);

#if TORRENT_USE_ASSERTS
		if (!m_queued_jobs.empty())
		{
			for (auto i = m_queued_jobs.iterate(); i.get(); i.next())
				std::printf("job: %d\n", int(i.get()->action.index()));
		}
		TORRENT_ASSERT(m_queued_jobs.empty());
#endif
	}

	void disk_io_thread_pool::set_max_threads(int const i)
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (i == m_max_threads) return;
		m_max_threads = i;
		if (int(m_threads.size()) < i) return;
		stop_threads(int(m_threads.size()) - i);
	}

	void disk_io_thread_pool::abort(bool wait)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		if (m_abort) return;
		m_abort = true;
		m_idle_timer.cancel();
		stop_threads(int(m_threads.size()));
		for (auto& t : m_threads)
		{
			if (wait)
			{
				// must release m_mutex to avoid a deadlock if the thread
				// tries to acquire it
				l.unlock();
				t.join();
				l.lock();
			}
			else
				t.detach();
		}
		m_threads.clear();
	}

	void disk_io_thread_pool::thread_active()
	{
		int const num_idle_threads = --m_num_idle_threads;
		TORRENT_ASSERT(num_idle_threads >= 0);

		int current_min = m_min_idle_threads;
		while (num_idle_threads < current_min
			&& !m_min_idle_threads.compare_exchange_weak(current_min, num_idle_threads));
	}

	bool disk_io_thread_pool::try_thread_exit(std::thread::id id)
	{
		int to_exit = m_threads_to_exit;
		while (to_exit > 0 &&
			!m_threads_to_exit.compare_exchange_weak(to_exit, to_exit - 1));
		if (to_exit > 0)
		{
			std::unique_lock<std::mutex> l(m_mutex);
			if (!m_abort)
			{
				auto new_end = std::remove_if(m_threads.begin(), m_threads.end()
					, [id](std::thread& t)
				{
					if (t.get_id() == id)
					{
						t.detach();
						return true;
					}
					return false;
				});
				TORRENT_ASSERT(new_end != m_threads.end());
				m_threads.erase(new_end, m_threads.end());
				if (m_threads.empty()) m_idle_timer.cancel();
			}
		}
		return to_exit > 0;
	}

	std::thread::id disk_io_thread_pool::first_thread_id()
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_threads.empty()) return {};
		return m_threads.front().get_id();
	}

	void disk_io_thread_pool::job_queued(int const queue_size)
	{
		// this check is not strictly necessary
		// but do it to avoid acquiring the mutex in the trivial case
		if (m_num_idle_threads >= queue_size) return;
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_abort) return;

		// reduce the number of threads requested to stop if we're going to need
		// them for these new jobs
		int to_exit = m_threads_to_exit;
		while (to_exit > std::max(0, m_num_idle_threads - queue_size) &&
			!m_threads_to_exit.compare_exchange_weak(to_exit
				, std::max(0, m_num_idle_threads - queue_size)));

		// now start threads until we either have enough to service
		// all queued jobs without blocking or hit the max
		for (int i = m_num_idle_threads
			; i < queue_size && int(m_threads.size()) < m_max_threads
			; ++i)
		{
			// if this is the first thread started, start the reaper timer
			if (m_threads.empty())
			{
				m_idle_timer.expires_after(reap_idle_threads_interval);
				m_idle_timer.async_wait([this](error_code const& ec) { reap_idle_threads(ec); });
			}

			// work keeps the io_context::run() call blocked from returning.
			// When shutting down, it's possible that the event queue is drained
			// before the disk_io_thread has posted its last callback. When this
			// happens, the io_context will have a pending callback from the
			// disk_io_thread, but the event loop is not running. this means
			// that the event is destructed after the disk_io_thread. If the
			// event refers to a disk buffer it will try to free it, but the
			// buffer pool won't exist anymore, and crash. This prevents that.
			m_threads.emplace_back(m_thread_fun, std::ref(*this), make_work_guard(m_ioc));
		}
	}

	void disk_io_thread_pool::reap_idle_threads(error_code const& ec)
	{
		// take the minimum number of idle threads during the last
		// sample period and request that many threads to exit
		if (ec) return;
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_abort) return;
		if (m_threads.empty()) return;
		m_idle_timer.expires_after(reap_idle_threads_interval);
		m_idle_timer.async_wait([this](error_code const& e) { reap_idle_threads(e); });
		int const min_idle = m_min_idle_threads.exchange(m_num_idle_threads);
		if (min_idle <= 0) return;
		// stop either the minimum number of idle threads or the number of threads
		// which must be stopped to get below the max, whichever is larger
		int const to_stop = std::max(min_idle, int(m_threads.size()) - m_max_threads);
		stop_threads(to_stop);
	}

	void disk_io_thread_pool::stop_threads(int num_to_stop)
	{
		m_threads_to_exit = num_to_stop;
		m_job_cond.notify_all();
	}

	bool disk_io_thread_pool::wait_for_job(std::unique_lock<std::mutex>& l)
	{
		TORRENT_ASSERT(l.owns_lock());

		// the thread should only go active if it is exiting or there is work to do
		// if the thread goes active on every wakeup it causes the minimum idle thread
		// count to be lower than it should be
		// for performance reasons we also want to avoid going idle and active again
		// if there is already work to do
		if (m_queued_jobs.empty())
		{
			thread_idle();

			do
			{
				// if the number of wanted threads is decreased,
				// we may stop this thread
				// when we're terminating the last thread, make sure
				// we finish up all queued jobs first
				if (should_exit()
					&& (m_queued_jobs.empty()
						|| num_threads() > 1)
					// try_thread_exit must be the last condition
					&& try_thread_exit(std::this_thread::get_id()))
				{
					// time to exit this thread.
					thread_active();
					return true;
				}

				using namespace std::literals::chrono_literals;
				m_job_cond.wait_for(l, 1s);
			} while (m_queued_jobs.empty());

			thread_active();
		}

		return false;
	}


} // namespace libtorrent::aux
