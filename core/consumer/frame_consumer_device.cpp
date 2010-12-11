#include "../StdAfx.h"

#ifdef _MSC_VER
#pragma warning (disable : 4244)
#endif

#include "frame_consumer_device.h"

#include "../format/video_format.h"
#include "../processor/write_frame.h"
#include "../processor/frame_processor_device.h"
#include "../../common/concurrency/executor.h"

#include <tbb/concurrent_queue.h>
#include <tbb/atomic.h>

#include <boost/date_time/posix_time/posix_time.hpp>

#include <boost/range/algorithm_ext/erase.hpp>
#include <boost/range/algorithm.hpp>

namespace caspar { namespace core {

class clock_sync
{
public:
	clock_sync() : time_(boost::posix_time::microsec_clock::local_time()){}

	void wait(double period)
	{				
		auto remaining = boost::posix_time::microseconds(static_cast<long>(period*1000000.0)) - (boost::posix_time::microsec_clock::local_time() - time_);
		if(remaining > boost::posix_time::microseconds(5000))
			boost::this_thread::sleep(remaining - boost::posix_time::microseconds(5000));
	}
private:
	boost::posix_time::ptime time_;
};

struct frame_consumer_device::implementation
{
public:
	implementation(const frame_processor_device_ptr& frame_processor, const video_format_desc& format_desc, const std::vector<frame_consumer_ptr>& consumers) 
		: frame_processor_(frame_processor), consumers_(consumers), fmt_(format_desc)
	{		
		std::vector<size_t> depths;
		boost::range::transform(consumers_, std::back_inserter(depths), std::mem_fn(&frame_consumer::buffer_depth));
		max_depth_ = *boost::range::max_element(depths);
		executor_.start();
		executor_.begin_invoke([=]{tick();});
	}
					
	void tick()
	{
		process(frame_processor_->receive());		
		if(!consumers_.empty())
			executor_.begin_invoke([=]{tick();});
	}

	void process(const read_frame& frame)
	{		
		buffer_.push_back(frame);

		clock_sync clock;
		
		boost::range::for_each(consumers_, [&](const frame_consumer_ptr& consumer)
		{
			size_t offset = max_depth_ - consumer->buffer_depth();
			if(offset < buffer_.size())
				consumer->send(*(buffer_.begin() + offset));
		});
			
		frame_consumer::sync_mode sync = frame_consumer::ready;
		boost::range::for_each(consumers_, [&](const frame_consumer_ptr& consumer)
		{
			try
			{
				size_t offset = max_depth_ - consumer->buffer_depth();
				if(offset >= buffer_.size())
					return;

				if(consumer->synchronize() == frame_consumer::clock)
					sync = frame_consumer::clock;
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
				boost::range::remove_erase(consumers_, consumer);
				CASPAR_LOG(warning) << "Removed consumer from frame_consumer_device.";
			}
		});
	
		if(sync != frame_consumer::clock)
			clock.wait(fmt_.period);

		if(buffer_.size() >= max_depth_)
			buffer_.pop_front();
	}

	common::executor executor_;	

	size_t max_depth_;
	std::deque<read_frame> buffer_;		

	std::vector<frame_consumer_ptr> consumers_;
	
	frame_processor_device_ptr frame_processor_;

	const video_format_desc& fmt_;
};

frame_consumer_device::frame_consumer_device(const frame_processor_device_ptr& frame_processor, const video_format_desc& format_desc, const std::vector<frame_consumer_ptr>& consumers)
	: impl_(new implementation(frame_processor, format_desc, consumers)){}
}}