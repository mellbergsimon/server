/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/
#include "stdafx.h"

#include "server.h"

#include <accelerator/accelerator.h>

#include <common/env.h>
#include <common/except.h>
#include <common/utf.h>
#include <common/memory.h>
#include <common/polling_filesystem_monitor.h>

#include <core/video_channel.h>
#include <core/video_format.h>
#include <core/producer/stage.h>
#include <core/producer/frame_producer.h>
#include <core/producer/scene/scene_producer.h>
#include <core/producer/scene/xml_scene_producer.h>
#include <core/producer/text/text_producer.h>
#include <core/consumer/output.h>
#include <core/thumbnail_generator.h>
#include <core/diagnostics/subject_diagnostics.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/osd_graph.h>

#include <modules/bluefish/bluefish.h>
#include <modules/decklink/decklink.h>
#include <modules/ffmpeg/ffmpeg.h>
#include <modules/flash/flash.h>
#include <modules/oal/oal.h>
#include <modules/screen/screen.h>
#include <modules/image/image.h>
#include <modules/image/consumer/image_consumer.h>
#include <modules/psd/psd_scene_producer.h>

#include <modules/oal/consumer/oal_consumer.h>
#include <modules/bluefish/consumer/bluefish_consumer.h>
#include <modules/decklink/consumer/decklink_consumer.h>
#include <modules/screen/consumer/screen_consumer.h>
#include <modules/ffmpeg/consumer/ffmpeg_consumer.h>

#include <protocol/asio/io_service_manager.h>
#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/cii/CIIProtocolStrategy.h>
#include <protocol/clk/CLKProtocolStrategy.h>
#include <protocol/util/AsyncEventServer.h>
#include <protocol/util/strategy_adapters.h>
#include <protocol/osc/client.h>

#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>

#include <future>

namespace caspar {

using namespace core;
using namespace protocol;

struct server::impl : boost::noncopyable
{
	protocol::asio::io_service_manager					io_service_manager_;
	spl::shared_ptr<monitor::subject>					monitor_subject_;
	spl::shared_ptr<monitor::subject>					diag_subject_					= core::diagnostics::get_or_create_subject();
	accelerator::accelerator							accelerator_;
	std::vector<spl::shared_ptr<IO::AsyncEventServer>>	async_servers_;
	std::shared_ptr<IO::AsyncEventServer>				primary_amcp_server_;
	osc::client											osc_client_;
	std::vector<std::shared_ptr<void>>					predefined_osc_subscriptions_;
	std::vector<spl::shared_ptr<video_channel>>			channels_;
	std::shared_ptr<thumbnail_generator>				thumbnail_generator_;
	std::promise<bool>&									shutdown_server_now_;

	explicit impl(std::promise<bool>& shutdown_server_now)		
		: accelerator_(env::properties().get(L"configuration.accelerator", L"auto"))
		, osc_client_(io_service_manager_.service())
		, shutdown_server_now_(shutdown_server_now)
	{
		core::diagnostics::osd::register_sink();
		diag_subject_->attach_parent(monitor_subject_);

		ffmpeg::init();
		CASPAR_LOG(info) << L"Initialized ffmpeg module.";
							  
		bluefish::init();	  
		CASPAR_LOG(info) << L"Initialized bluefish module.";
							  
		decklink::init();	  
		CASPAR_LOG(info) << L"Initialized decklink module.";
							  							  
		oal::init();		  
		CASPAR_LOG(info) << L"Initialized oal module.";
							  
		screen::init();		  
		CASPAR_LOG(info) << L"Initialized ogl module.";

		image::init();		  
		CASPAR_LOG(info) << L"Initialized image module.";

		flash::init();		  
		CASPAR_LOG(info) << L"Initialized flash module.";

		psd::init();		  
		CASPAR_LOG(info) << L"Initialized psd module.";

		core::text::init();

		register_producer_factory(&core::scene::create_dummy_scene_producer);
		register_producer_factory(&core::scene::create_xml_scene_producer);

		setup_channels(env::properties());
		CASPAR_LOG(info) << L"Initialized channels.";

		setup_thumbnail_generation(env::properties());
		CASPAR_LOG(info) << L"Initialized thumbnail generator.";

		setup_controllers(env::properties());
		CASPAR_LOG(info) << L"Initialized controllers.";

		setup_osc(env::properties());
		CASPAR_LOG(info) << L"Initialized osc.";
	}

	~impl()
	{
		thumbnail_generator_.reset();
		primary_amcp_server_.reset();
		async_servers_.clear();
		channels_.clear();

		boost::this_thread::sleep(boost::posix_time::milliseconds(500));
		//Sleep(500); // HACK: Wait for asynchronous destruction of producers and consumers.

		image::uninit();
		ffmpeg::uninit();
		core::diagnostics::osd::shutdown();
	}
				
	void setup_channels(const boost::property_tree::wptree& pt)
	{   
		using boost::property_tree::wptree;
		for (auto& xml_channel : pt.get_child(L"configuration.channels"))
		{		
			auto format_desc = video_format_desc(xml_channel.second.get(L"video-mode", L"PAL"));		
			if(format_desc.format == video_format::invalid)
				CASPAR_THROW_EXCEPTION(caspar_exception() << msg_info("Invalid video-mode."));
			
			auto channel = spl::make_shared<video_channel>(static_cast<int>(channels_.size()+1), format_desc, accelerator_.create_image_mixer());

			core::diagnostics::scoped_call_context save;
			core::diagnostics::call_context::for_thread().video_channel = channel->index();
			
			for (auto& xml_consumer : xml_channel.second.get_child(L"consumers"))
			{
				try
				{
					auto name = xml_consumer.first;
					if(name == L"screen")
						channel->output().add(caspar::screen::create_consumer(xml_consumer.second, &channel->stage()));					
					else if(name == L"bluefish")					
						channel->output().add(bluefish::create_consumer(xml_consumer.second));					
					else if(name == L"decklink")					
						channel->output().add(decklink::create_consumer(xml_consumer.second));				
					else if(name == L"file")					
						channel->output().add(ffmpeg::create_consumer(xml_consumer.second));						
					else if(name == L"system-audio")
						channel->output().add(oal::create_consumer());		
					else if(name != L"<xmlcomment>")
						CASPAR_LOG(warning) << "Invalid consumer: " << name;	
				}
				catch(...)
				{
					CASPAR_LOG_CURRENT_EXCEPTION();
				}
			}		

		    channel->monitor_output().attach_parent(monitor_subject_);
			channels_.push_back(channel);
		}

		// Dummy diagnostics channel
		if(env::properties().get(L"configuration.channel-grid", false))
			channels_.push_back(spl::make_shared<video_channel>(static_cast<int>(channels_.size()+1), core::video_format_desc(core::video_format::x576p2500), accelerator_.create_image_mixer()));
	}

	void setup_osc(const boost::property_tree::wptree& pt)
	{		
		using boost::property_tree::wptree;
		using namespace boost::asio::ip;

		monitor_subject_->attach_parent(osc_client_.sink());
		
		auto default_port =
				pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
		auto predefined_clients =
				pt.get_child_optional(L"configuration.osc.predefined-clients");

		if (predefined_clients)
		{
			for (auto& predefined_client : *predefined_clients)
			{
				const auto address =
						predefined_client.second.get<std::wstring>(L"address");
				const auto port =
						predefined_client.second.get<unsigned short>(L"port");
				predefined_osc_subscriptions_.push_back(
						osc_client_.get_subscription_token(udp::endpoint(
								address_v4::from_string(u8(address)),
								port)));
			}
		}

		if (primary_amcp_server_)
			primary_amcp_server_->add_client_lifecycle_object_factory(
					[=] (const std::string& ipv4_address)
							-> std::pair<std::wstring, std::shared_ptr<void>>
					{
						using namespace boost::asio::ip;

						return std::make_pair(
								std::wstring(L"osc_subscribe"),
								osc_client_.get_subscription_token(
										udp::endpoint(
												address_v4::from_string(
														ipv4_address),
												default_port)));
					});
	}

	void setup_thumbnail_generation(const boost::property_tree::wptree& pt)
	{
		if (!pt.get(L"configuration.thumbnails.generate-thumbnails", true))
			return;

		auto scan_interval_millis = pt.get(L"configuration.thumbnails.scan-interval-millis", 5000);

		polling_filesystem_monitor_factory monitor_factory(scan_interval_millis);
		thumbnail_generator_.reset(new thumbnail_generator(
			monitor_factory, 
			env::media_folder(),
			env::thumbnails_folder(),
			pt.get(L"configuration.thumbnails.width", 256),
			pt.get(L"configuration.thumbnails.height", 144),
			core::video_format_desc(pt.get(L"configuration.thumbnails.video-mode", L"720p2500")),
			accelerator_.create_image_mixer(),
			pt.get(L"configuration.thumbnails.generate-delay-millis", 2000),
			&image::write_cropped_png));

		CASPAR_LOG(info) << L"Initialized thumbnail generator.";
	}
		
	void setup_controllers(const boost::property_tree::wptree& pt)
	{		
		using boost::property_tree::wptree;
		for (auto& xml_controller : pt.get_child(L"configuration.controllers"))
		{
			try
			{
				auto name = xml_controller.first;
				auto protocol = xml_controller.second.get<std::wstring>(L"protocol");	

				if(name == L"tcp")
				{					
					unsigned int port = xml_controller.second.get(L"port", 5250);
					auto asyncbootstrapper = spl::make_shared<IO::AsyncEventServer>(create_protocol(protocol), port);
					async_servers_.push_back(asyncbootstrapper);

					if (!primary_amcp_server_ && boost::iequals(protocol, L"AMCP"))
						primary_amcp_server_ = asyncbootstrapper;
				}
				else
					CASPAR_LOG(warning) << "Invalid controller: " << name;	
			}
			catch(...)
			{
				CASPAR_LOG_CURRENT_EXCEPTION();
			}
		}
	}

	IO::protocol_strategy_factory<char>::ptr create_protocol(const std::wstring& name) const
	{
		using namespace IO;

		if(boost::iequals(name, L"AMCP"))
			return wrap_legacy_protocol("\r\n", spl::make_shared<amcp::AMCPProtocolStrategy>(channels_, thumbnail_generator_, shutdown_server_now_));
		else if(boost::iequals(name, L"CII"))
			return wrap_legacy_protocol("\r\n", spl::make_shared<cii::CIIProtocolStrategy>(channels_));
		else if(boost::iequals(name, L"CLOCK"))
			return spl::make_shared<to_unicode_adapter_factory>(
					"ISO-8859-1",
					spl::make_shared<CLK::clk_protocol_strategy_factory>(channels_));
		
		CASPAR_THROW_EXCEPTION(caspar_exception() << arg_name_info(L"name") << arg_value_info(name) << msg_info(L"Invalid protocol"));
	}

};

server::server(std::promise<bool>& shutdown_server_now) : impl_(new impl(shutdown_server_now)){}

const std::vector<spl::shared_ptr<video_channel>> server::channels() const
{
	return impl_->channels_;
}
std::shared_ptr<core::thumbnail_generator> server::get_thumbnail_generator() const {return impl_->thumbnail_generator_; }
core::monitor::subject& server::monitor_output() { return *impl_->monitor_subject_; }

}
