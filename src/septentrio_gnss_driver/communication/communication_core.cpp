// *****************************************************************************
//
// © Copyright 2020, Septentrio NV/SA.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//    1. Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//    2. Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//    3. Neither the name of the copyright holder nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// *****************************************************************************

#include <chrono>

// Boost includes
#include <boost/regex.hpp>

#include <septentrio_gnss_driver/communication/communication_core.hpp>
#include <septentrio_gnss_driver/communication/pcap_reader.hpp>

#ifndef ANGLE_MAX
#define ANGLE_MAX 180
#endif

#ifndef ANGLE_MIN
#define ANGLE_MIN -180
#endif

#ifndef THETA_Y_MAX
#define THETA_Y_MAX 90
#endif

#ifndef THETA_Y_MIN
#define THETA_Y_MIN -90
#endif

#ifndef LEVER_ARM_MAX
#define LEVER_ARM_MAX 100
#endif

#ifndef LEVER_ARM_MIN
#define LEVER_ARM_MIN -100
#endif

#ifndef HEADING_MAX
#define HEADING_MAX 360
#endif

#ifndef HEADING_MIN
#define HEADING_MIN -360
#endif

#ifndef PITCH_MAX
#define PITCH_MAX 90
#endif

#ifndef PITCH_MIN
#define PITCH_MIN -90
#endif

#ifndef ATTSTD_DEV_MIN
#define ATTSTD_DEV_MIN 0
#endif

#ifndef ATTSTD_DEV_MAX
#define ATTSTD_DEV_MAX 5
#endif

#ifndef POSSTD_DEV_MIN
#define POSSTD_DEV_MIN 0
#endif

#ifndef POSSTD_DEV_MAX
#define POSSTD_DEV_MAX 100
#endif

/**
 * @file communication_core.cpp
 * @date 22/08/20
 * @brief Highest-Level view on communication services
 */

//! Mutex to control changes of global variable "g_response_received"
boost::mutex g_response_mutex;
//! Determines whether a command reply was received from the Rx
bool g_response_received;
//! Condition variable complementing "g_response_mutex"
boost::condition_variable g_response_condition;
//! Mutex to control changes of global variable "g_cd_received"
boost::mutex g_cd_mutex;
//! Determines whether the connection descriptor was received from the Rx
bool g_cd_received;
//! Condition variable complementing "g_cd_mutex"
boost::condition_variable g_cd_condition;
//! Whether or not we still want to read the connection descriptor, which we only
//! want in the very beginning to know whether it is IP10, IP11 etc.
bool g_read_cd;
//! Rx TCP port, e.g. IP10 or IP11, to which ROSaic is connected to
std::string g_rx_tcp_port;
//! Since after SSSSSSSSSSS we need to wait for second connection descriptor, we have
//! to count the connection descriptors
uint32_t g_cd_count;

template<class NodeT>
io_comm_rx::Comm_IO<NodeT>::Comm_IO(NodeT node, std::shared_ptr<ros::NodeHandle> pNh, Settings* settings) : 
    node_(node),
    handlers_(pNh, settings),
    settings_(settings),
    stopping_(false)
{
    g_response_received = false;
    g_cd_received = false;
    g_read_cd = true;
    g_cd_count = 0;
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::initializeIO()
{
    node_->log(LogLevel::DEBUG, "Called initializeIO() method");
    boost::smatch match;
    // In fact: smatch is a typedef of match_results<string::const_iterator>
    if (boost::regex_match(settings_->device, match, boost::regex("(tcp)://(.+):(\\d+)")))
    // C++ needs \\d instead of \d: Both mean decimal.
    // Note that regex_match can be used with a smatch object to store results, or
    // without. In any case, true is returned if and only if it matches the
    // !complete! string.
    {
        // The first sub_match (index 0) contained in a match_result always
        // represents the full match within a target sequence made by a regex, and
        // subsequent sub_matches represent sub-expression matches corresponding in
        // sequence to the left parenthesis delimiting the sub-expression in the
        // regex, i.e. $n Perl is equivalent to m[n] in boost regex.
        tcp_host_ = match[2];
        tcp_port_ = match[3];

        serial_ = false;
        settings_->read_from_sbf_log = false;
        settings_->read_from_pcap = false;
        connectionThread_.reset(new  boost::thread (boost::bind(&Comm_IO<NodeT>::connect, this)));
    } else if (boost::regex_match(settings_->device, match,
                                  boost::regex("(file_name):(/|(?:/[\\w-]+)+.sbf)")))
    {
        serial_ = false;
        settings_->read_from_sbf_log = true;
        settings_->read_from_pcap = false;
        settings_->use_gnss_time = true;
        connectionThread_.reset(new  boost::thread (
            boost::bind(&Comm_IO<NodeT>::prepareSBFFileReading, this, match[2])));

    } else if (boost::regex_match(
                   settings_->device, match,
                   boost::regex("(file_name):(/|(?:/[\\w-]+)+.pcap)")))
    {
        serial_ = false;
        settings_->read_from_sbf_log = false;
        settings_->read_from_pcap = true;
        settings_->use_gnss_time = true;
        connectionThread_.reset(new  boost::thread (
            boost::bind(&Comm_IO<NodeT>::preparePCAPFileReading, this, match[2])));

    } else if (boost::regex_match(settings_->device, match, boost::regex("(serial):(.+)")))
    {
        serial_ = true;
        settings_->read_from_sbf_log = false;
        settings_->read_from_pcap = false;
        std::string proto(match[2]);
        std::stringstream ss;
        ss << "Searching for serial port" << proto;
		settings_->device = proto;
        node_->log(LogLevel::DEBUG, ss.str());
        connectionThread_.reset(new  boost::thread (boost::bind(&Comm_IO<NodeT>::connect, this)));
    } else
    {
        std::stringstream ss;
        ss << "Device is unsupported. Perhaps you meant 'tcp://host:port' or 'file_name:xxx.sbf' or 'serial:/path/to/device'?";
        node_->log(LogLevel::ERROR, ss.str());
    }
    node_->log(LogLevel::DEBUG, "Leaving initializeIO() method");
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::prepareSBFFileReading(std::string file_name)
{
    try
    {
        std::stringstream ss;
        ss << "Setting up everything needed to read from" << file_name;
        node_->log(LogLevel::DEBUG, ss.str());
        initializeSBFFileReading(file_name);
    } catch (std::runtime_error& e)
    {
        std::stringstream ss;
        ss << "Comm_IO<NodeT>::initializeSBFFileReading() failed for SBF File" << file_name
           << " due to: " << e.what();
        node_->log(LogLevel::ERROR, ss.str());
    }
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::preparePCAPFileReading(std::string file_name)
{
    try
    {
        std::stringstream ss;
        ss << "Setting up everything needed to read from " << file_name;
        node_->log(LogLevel::DEBUG, ss.str());
        initializePCAPFileReading(file_name);
    } catch (std::runtime_error& e)
    {
        std::stringstream ss;
        ss << "CommIO::initializePCAPFileReading() failed for SBF File " << file_name
           << " due to: " << e.what();
        node_->log(LogLevel::ERROR, ss.str());
    }
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::connect()
{
    node_->log(LogLevel::DEBUG, "Called connect() method");
    node_->log(LogLevel::DEBUG, 
        "Started timer for calling reconnect() method until connection succeeds");

    boost::asio::io_service io;
    boost::posix_time::millisec wait_ms(static_cast<uint32_t>(settings_->reconnect_delay_s * 1000));
    while (!connected_ && !stopping_)
    {
        boost::asio::deadline_timer t(io, wait_ms);
        reconnect();
        t.wait();
    }
    node_->log(LogLevel::DEBUG, "Successully connected. Leaving connect() method");
}

//! In serial mode (not USB, since the Rx port is then called USB1 or USB2), please
//! ensure that you are connected to the Rx's COM1, COM2 or COM3 port, !if! you
//! employ UART hardware flow control.
template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::reconnect()
{
    node_->log(LogLevel::DEBUG, "Called reconnect() method");
    if (serial_)
    {
        bool initialize_serial_return = false;
        try
        {
            node_->log(LogLevel::INFO, "Connecting serially to device" + settings_->device + 
                                       ", targeted baudrate: " + std::to_string(settings_->baudrate));
            initialize_serial_return =
                initializeSerial(settings_->device, settings_->baudrate, settings_->hw_flow_control);
        } catch (std::runtime_error& e)
        {
            {
                std::stringstream ss;
                ss << "initializeSerial() failed for device " << settings_->device
                    << " due to: " << e.what();
                node_->log(LogLevel::ERROR, ss.str());
            }
        }
        if (initialize_serial_return)
        {
            boost::mutex::scoped_lock lock(connection_mutex_);
            connected_ = true;
            lock.unlock();
            connection_condition_.notify_one();
        }
    } else
    {
        bool initialize_tcp_return = false;
        try
        {
            node_->log(LogLevel::INFO, "Connecting to tcp://" + tcp_host_ + ":" + tcp_port_ +
                                       "...");
            initialize_tcp_return = initializeTCP(tcp_host_, tcp_port_);
        } catch (std::runtime_error& e)
        {
            {
                std::stringstream ss;
                ss << "initializeTCP() failed for host " << tcp_host_
                    << " on port " << tcp_port_ << " due to: " << e.what();
                node_->log(LogLevel::ERROR, ss.str());
            }
        }
        if (initialize_tcp_return)
        {
            boost::mutex::scoped_lock lock(connection_mutex_);
            connected_ = true;
            lock.unlock();
            connection_condition_.notify_one();
        }
    }
    node_->log(LogLevel::DEBUG, "Leaving reconnect() method");
}

//! The send() method of AsyncManager class is paramount for this purpose.
//! Note that std::to_string() is from C++11 onwards only.
//! Since ROSaic can be launched before booting the Rx, we have to watch out for
//! escape characters that are sent by the Rx to indicate that it is in upgrade mode.
//! Those characters would then be mingled with the first command we send to it in
//! this method and could result in an invalid command. Hence we first enter command
//! mode via "SSSSSSSSSS".
template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::configureRx()
{
    node_->log(LogLevel::DEBUG, "Called configureRx() method");
    {
        // wait for connection
        boost::mutex::scoped_lock lock(connection_mutex_);
        connection_condition_.wait(lock, [this]() { return connected_; });
    }       

    

    // Determining communication mode: TCP vs USB/Serial
    unsigned stream = 1;
    boost::smatch match;
    boost::regex_match(settings_->device, match, boost::regex("(tcp)://(.+):(\\d+)"));
    std::string proto(match[1]);
    std::string rx_port;
    if (proto == "tcp")
    {
        // It is imperative to hold a lock on the mutex  "g_cd_mutex" while
        // modifying the variable and "g_cd_received".
        boost::mutex::scoped_lock lock_cd(g_cd_mutex);
        // Escape sequence (escape from correction mode), ensuring that we can send
        // our real commands afterwards...
        std::string cmd("\x0DSSSSSSSSSSSSSSSSSSS\x0D\x0D");
        manager_.get()->send(cmd, cmd.size());
        // We wait for the connection descriptor before we send another command,
        // otherwise the latter would not be processed.
        g_cd_condition.wait(lock_cd, []() { return g_cd_received; });
        g_cd_received = false;
        rx_port = g_rx_tcp_port;
    } else
    {
        rx_port = settings_->rx_serial_port;
        // After booting, the Rx sends the characters "x?" to all ports, which could
        // potentially mingle with our first command. Hence send a safeguard command
        // "lif", whose potentially false processing is harmless.
        send("lif, Identification \x0D");
    }
    uint32_t rx_period_pvt =
        parsing_utilities::convertUserPeriodToRxCommand(settings_->polling_period_pvt);
    uint32_t rx_period_rest =
        parsing_utilities::convertUserPeriodToRxCommand(settings_->polling_period_rest);
    std::string pvt_sec_or_msec;
    std::string rest_sec_or_msec;
    if (settings_->polling_period_pvt == 1000 || settings_->polling_period_pvt == 2000 ||
        settings_->polling_period_pvt == 5000 || settings_->polling_period_pvt == 10000)
        pvt_sec_or_msec = "sec";
    else
        pvt_sec_or_msec = "msec";
    if (settings_->polling_period_rest == 1000 || settings_->polling_period_rest == 2000 ||
        settings_->polling_period_rest == 5000 || settings_->polling_period_rest == 10000)
        rest_sec_or_msec = "sec";
    else
        rest_sec_or_msec = "msec";

    // Turning off all current SBF/NMEA output    
    send("sso, all, none, none, off \x0D");
    send("sno, all, none, none, off \x0D");

    // Setting the datum to be used by the Rx (not the NMEA output though, which only
    // provides MSL and undulation (by default with respect to WGS84), but not
    // ellipsoidal height)
    {
        std::stringstream ss;
        ss << "sgd, " << settings_->datum << "\x0D";
        send(ss.str());
    }

    // Setting SBF/NMEA output of Rx depending on the receiver type
    // If GNSS then...
    if (settings_->septentrio_receiver_type == "gnss")
    {
        if (settings_->publish_pvtcartesian == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", PVTCartesian, " << pvt_sec_or_msec << std::to_string(rx_period_pvt)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_pvtgeodetic == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", PVTGeodetic, " << pvt_sec_or_msec << std::to_string(rx_period_pvt)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_poscovcartesian == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", PosCovCartesian, " << pvt_sec_or_msec
            << std::to_string(rx_period_pvt) << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_poscovgeodetic == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", PosCovGeodetic, " << pvt_sec_or_msec
            << std::to_string(rx_period_pvt) << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_velcovgeodetic == true)
        {
            std::stringstream ss;
            ss.str(std::string());
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", VelCovGeodetic, " << pvt_sec_or_msec
            << std::to_string(rx_period_pvt) << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_atteuler == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", AttEuler, " << rest_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_attcoveuler == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", AttCovEuler, " << rest_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
    }
    // Setting SBF/NMEA output of Rx depending on the receiver type
    // If INS then...
    if (settings_->septentrio_receiver_type == "ins")
    {
        if (settings_->publish_insnavcart == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", INSNavCart, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_insnavgeod == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", INSNavGeod, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_imusetup == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", IMUSetup, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_velsensorsetup == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", VelSensorSetup, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_exteventinsnavgeod == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", ExtEventINSNavGeod, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_exteventinsnavcart == true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", ExtEventINSNavCart, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
        if (settings_->publish_extsensormeas== true)
        {
            std::stringstream ss;
            ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
            << ", ExtSensorMeas, " << pvt_sec_or_msec << std::to_string(rx_period_rest)
            << "\x0D";
            send(ss.str());
            ++stream;
        }
    }
	if (settings_->publish_gpgga == true)
	{
		std::stringstream ss;

		ss << "sno, Stream" << std::to_string(stream) << ", " << rx_port << ", GGA, "
		<< pvt_sec_or_msec << std::to_string(rx_period_pvt) << "\x0D";
		send(ss.str());
		++stream;
	}
	if (settings_->publish_gprmc == true)
	{
		std::stringstream ss;

		ss << "sno, Stream" << std::to_string(stream) << ", " << rx_port << ", RMC, "
		<< pvt_sec_or_msec << std::to_string(rx_period_pvt) << "\x0D";
		send(ss.str());
		++stream;
	}
	if (settings_->publish_gpgsa == true)
	{
		std::stringstream ss;

		ss << "sno, Stream" << std::to_string(stream) << ", " << rx_port << ", GSA, "
		<< pvt_sec_or_msec << std::to_string(rx_period_pvt) << "\x0D";
		send(ss.str());
		++stream;
	}
	if (settings_->publish_gpgsv == true)
	{
		std::stringstream ss;

		ss << "sno, Stream" << std::to_string(stream) << ", " << rx_port << ", GSV, "
		<< rest_sec_or_msec << std::to_string(rx_period_rest) << "\x0D";
		send(ss.str());
		++stream;
	}
	if (settings_->publish_gpsfix == true)
	{
		std::stringstream ss;
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
		<< ", ChannelStatus, " << pvt_sec_or_msec << std::to_string(rx_period_pvt)
		<< "\x0D";
		send(ss.str());
		++stream;
		ss.str(std::string()); // avoids invoking the std::string constructor
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
		<< ", MeasEpoch, " << pvt_sec_or_msec << std::to_string(rx_period_pvt)
		<< "\x0D";
		send(ss.str());
		++stream;
		ss.str(std::string());
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port << ", DOP, "
		<< pvt_sec_or_msec << std::to_string(rx_period_pvt) << "\x0D";
		send(ss.str());
		++stream;
	}  
	if (settings_->publish_diagnostics == true)
	{
		std::stringstream ss;
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
		<< ", ReceiverStatus, " << rest_sec_or_msec
		<< std::to_string(rx_period_rest) << "\x0D";
		send(ss.str());
		++stream;
		ss.str(std::string());
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
		<< ", QualityInd, " << rest_sec_or_msec << std::to_string(rx_period_rest)
		<< "\x0D";
		send(ss.str());
		++stream;
		ss.str(std::string());
		ss << "sso, Stream" << std::to_string(stream) << ", " << rx_port
		<< ", ReceiverSetup, " << rest_sec_or_msec
		<< std::to_string(rx_period_rest) << "\x0D";
		send(ss.str());
		++stream;
	}

    // Setting the marker-to-ARP offsets. This comes after the "sso, ...,
    // ReceiverSetup, ..." command, since the latter is only generated when a
    // user-command is entered to change one or more values in the block.
    {
        std::stringstream ss;
        ss << "sao, Main, " << string_utilities::trimString(std::to_string(settings_->delta_e))
           << ", " << string_utilities::trimString(std::to_string(settings_->delta_n)) << ", "
           << string_utilities::trimString(std::to_string(settings_->delta_u)) << ", \""
           << settings_->ant_type << "\", " << settings_->ant_serial_nr << "\x0D";
        send(ss.str());
    }

    // Configure Aux1 antenna
    {
        std::stringstream ss;
        ss << "sao, Aux1, " << string_utilities::trimString(std::to_string(settings_->delta_aux1_e))
           << ", " << string_utilities::trimString(std::to_string(settings_->delta_aux1_n)) << ", "
           << string_utilities::trimString(std::to_string(settings_->delta_aux1_u)) << ", \""
           << settings_->ant_aux1_type << "\", " << settings_->ant_aux1_serial_nr << "\x0D";
        send(ss.str());
    }

    // Configuring the NTRIP connection
    // First disable any existing NTRIP connection on NTR1
    {
        std::stringstream ss;
        ss << "snts, NTR1, off \x0D";
        send(ss.str());
    }
    if (settings_->rx_has_internet)
    {
        if (settings_->ntrip_mode == "off")
        {
        } else if (settings_->ntrip_mode == "Client")
        {
            {
                std::stringstream ss;
                ss << "snts, NTR1, " << settings_->ntrip_mode << ", " << settings_->caster << ", "
                   << std::to_string(settings_->caster_port) << ", " << settings_->ntrip_username << ", "
                   << settings_->ntrip_password << ", " << settings_->mountpoint << ", " << settings_->ntrip_version
                   << ", " << settings_->send_gga << " \x0D";
                send(ss.str());
            }
        } else if (settings_->ntrip_mode == "Client-Sapcorda")
        {
            {
                std::stringstream ss;
                ss << "snts, NTR1, Client-Sapcorda, , , , , , , , \x0D";
                send(ss.str());
            }
        } else
        {
            node_->log(LogLevel::ERROR, "Invalid mode specified for NTRIP settings_->");
        }
    } else // Since the Rx does not have internet (and you will not be able to share
           // it via USB),
           // we need to forward the corrections ourselves, though not on the same
           // port.
    {
        if (proto == "tcp")
        {
            {
                std::stringstream ss;
                // In case IPS1 was used before, old configuration is lost of course.
                ss << "siss, IPS1, " << std::to_string(settings_->rx_input_corrections_tcp)
                   << ", TCP2Way \x0D";
                send(ss.str());
            }
            {
                std::stringstream ss;
                ss << "sno, Stream" << std::to_string(stream) << ", IPS1, GGA, "
                   << pvt_sec_or_msec << std::to_string(rx_period_pvt) << " \x0D";
                ++stream;
                send(ss.str());
            }
        }
        {
            std::stringstream ss;
            if (proto == "tcp")
            {
                ss << "sdio, IPS1, " << settings_->rtcm_version << ", +SBF+NMEA \x0D";
            } else
            {
                ss << "sdio, " << settings_->rx_input_corrections_serial << ", "
                   << settings_->rtcm_version << ", +SBF+NMEA \x0D";
            }
            send(ss.str());
        }
    }
    
	// Setting the INS-related commands
    if (settings_->septentrio_receiver_type == "ins")
    {
        // IMU orientation 
        {
            std::string orientation_mode;
            std::stringstream ss;
			if (settings_->manual == false)
			{
				orientation_mode = "SensorDefault";
			}
			else
			{
				orientation_mode = "manual";
			}
			if (settings_->theta_x >= ANGLE_MIN && settings_->theta_x<= ANGLE_MAX && settings_->theta_y >= THETA_Y_MIN && 
                settings_->theta_y <= THETA_Y_MAX && settings_->theta_z >= ANGLE_MIN && settings_->theta_z <= ANGLE_MAX)
			{
				ss << " sio, " << orientation_mode << ", " << string_utilities::trimString(std::to_string(settings_->theta_x)) << ", " 
					<< string_utilities::trimString(std::to_string(settings_->theta_y)) << ", " 
					<< string_utilities::trimString(std::to_string(settings_->theta_z)) << " \x0D";
				send(ss.str());
			}
			else
			{
				node_->log(LogLevel::ERROR, "Please specify a correct value for IMU orientation angles");
			}
        
        }

        // Setting the INS antenna lever arm offset
        {
            if (settings_->ant_lever_x>=LEVER_ARM_MIN && settings_->ant_lever_x<=LEVER_ARM_MAX && settings_->ant_lever_y>=LEVER_ARM_MIN &&
                settings_->ant_lever_y<=LEVER_ARM_MAX && settings_->ant_lever_z>=LEVER_ARM_MIN && settings_->ant_lever_z<=LEVER_ARM_MAX)
            {
                std::stringstream ss;
                ss << "sial, " << string_utilities::trimString(std::to_string(settings_->ant_lever_x))
                << ", " << string_utilities::trimString(std::to_string(settings_->ant_lever_y)) << ", "
                << string_utilities::trimString(std::to_string(settings_->ant_lever_z)) << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Please specify a correct value for x, y and z in the config file under ant_lever_arm");
            }
        }

        // Setting the user defined point offset
        {
            if (settings_->poi_x>=LEVER_ARM_MIN && settings_->poi_x<=LEVER_ARM_MAX && settings_->poi_y>=LEVER_ARM_MIN &&
                settings_->poi_y<=LEVER_ARM_MAX && settings_->poi_z>=LEVER_ARM_MIN && settings_->poi_z<=LEVER_ARM_MAX)
            {
                std::stringstream ss;
                ss << "sipl, POI1, " << string_utilities::trimString(std::to_string(settings_->poi_x))
                << ", " << string_utilities::trimString(std::to_string(settings_->poi_y)) << ", "
                << string_utilities::trimString(std::to_string(settings_->poi_z)) << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Please specify a correct value for poi_x, poi_y and poi_z in the config file under poi_to_imu");
            }
        }

        // Setting the Velocity sensor lever arm offset
        {
            if (settings_->vsm_x>=LEVER_ARM_MIN && settings_->vsm_x<=LEVER_ARM_MAX && settings_->vsm_y>=LEVER_ARM_MIN &&
                settings_->vsm_y<=LEVER_ARM_MAX && settings_->vsm_z>=LEVER_ARM_MIN && settings_->vsm_z<=LEVER_ARM_MAX)
            {
                std::stringstream ss;
                ss << "sivl, VSM1, " << string_utilities::trimString(std::to_string(settings_->vsm_x))
                << ", " << string_utilities::trimString(std::to_string(settings_->vsm_y)) << ", "
                << string_utilities::trimString(std::to_string(settings_->vsm_z)) << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Please specify a correct value for vsm_x, vsm_y and vsm_z in the config file under vel_sensor_lever_arm");
            }
            
        }

        // Setting the Attitude Determination
        {
            if (settings_->heading_offset >= HEADING_MIN && settings_->heading_offset<= HEADING_MAX && settings_->pitch_offset >= PITCH_MIN && settings_->pitch_offset <= PITCH_MAX)
            {
                std::stringstream ss;
                ss << "sto, " << string_utilities::trimString(std::to_string(settings_->heading_offset))
                << ", " << string_utilities::trimString(std::to_string(settings_->pitch_offset)) << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Please specify a valid parameter for heading and pitch");
            }
        }
        
        // Setting the INS Solution Reference Point: MainAnt or POI1
        // First disable any existing INS sub-block connection
        {
            std::stringstream ss;
            ss << "sinc, off, all, MainAnt \x0D";
            send(ss.str());
        }
		
		// INS solution reference point
		{
			std::stringstream ss;
			std::string insnavconfig;
			insnavconfig = " PosStdDev ";
			insnavconfig += "+Att";
			insnavconfig += "+AttStdDev";
			insnavconfig += "+Vel";
			insnavconfig += "+VelStdDev";
			if (settings_->ins_use_poi == true)
			{
				ss << "sinc, on, " << string_utilities::trimString(insnavconfig) << ", " << "POI1" << " \x0D";
				send(ss.str());
			}
			else
			{
				ss << "sinc, on, " << string_utilities::trimString(insnavconfig) << ", " << "MainAnt" << " \x0D";
				send(ss.str());
			}
		}

        // Setting the INS heading
        {
            std::stringstream ss;
            if (settings_->ins_initial_heading == "auto")
            {
                ss << "siih, " << settings_->ins_initial_heading << " \x0D";
                send(ss.str());
            }
            else if (settings_->ins_initial_heading == "stored")
            {
                ss << "siih, " << settings_->ins_initial_heading << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Invalid mode specified for ins_initial_heading.");
            }
        }

        // Setting the INS navigation filter
        {
            if (settings_->att_std_dev >=ATTSTD_DEV_MIN && settings_->att_std_dev <= ATTSTD_DEV_MAX && 
                settings_->pos_std_dev >= POSSTD_DEV_MIN && settings_->pos_std_dev <= POSSTD_DEV_MAX)
            {
                std::stringstream ss;
                ss << "sism, " << string_utilities::trimString(std::to_string(settings_->att_std_dev)) << ", " << string_utilities::trimString(std::to_string(settings_->pos_std_dev))
                << " \x0D";
                send(ss.str());
            }
            else
            {
                node_->log(LogLevel::ERROR, "Please specify a valid AttStsDev and PosStdDev");
            }
        }
    }
	node_->log(LogLevel::DEBUG, "Leaving configureRx() method");
}

//! initializeSerial is not self-contained: The for loop in Callbackhandlers' handle
//! method would never open a specific handler unless the handler is added
//! (=inserted) to the C++ map via this function. This way, the specific handler can
//! be called, in which in turn RxMessage's read() method is called, which publishes
//! the ROS message.
template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::defineMessages()
{
    node_->log(LogLevel::DEBUG, "Called defineMessages() method");

    if (settings_->publish_gpgga == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgga>("$GPGGA");
    }
    if (settings_->publish_gprmc == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gprmc>("$GPRMC");
    }
    if (settings_->publish_gpgsa == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgsa>("$GPGSA");
    }
    if (settings_->publish_gpgsv == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgsv>("$GPGSV");
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgsv>("$GLGSV");
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgsv>("$GAGSV");
        handlers_.callbackmap_ =
            handlers_.insert<septentrio_gnss_driver::Gpgsv>("$GBGSV");
    }
    if (settings_->publish_pvtcartesian == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<PVTCartesianMsg>("4006");
    }
    if (settings_->publish_pvtgeodetic == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<PVTGeodeticMsg>("4007");
    }
    if (settings_->publish_poscovcartesian == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<PosCovCartesianMsg>("5905");
    }
    if (settings_->publish_poscovgeodetic == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<PosCovGeodeticMsg>("5906");
    }
    if (settings_->publish_velcovgeodetic == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<VelCovGeodeticMsg>("5908");
    }
    if (settings_->publish_atteuler == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<AttEulerMsg>("5938");
    }
    if (settings_->publish_attcoveuler == true)
    {
        handlers_.callbackmap_ =
            handlers_.insert<AttCovEulerMsg>("5939");
    }
    
	// INS-related SBF blocks
    if (settings_->publish_insnavcart == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<INSNavCartMsg>("4225");
    }
    if (settings_->publish_insnavgeod == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<INSNavGeodMsg>("4226");
    }
    if (settings_->publish_imusetup == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<IMUSetupMsg>("4224");
    }
    if (settings_->publish_extsensormeas == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<ExtSensorMeasMsg>("4050");
    }
    if (settings_->publish_exteventinsnavgeod == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<ExtEventINSNavGeodMsg>("4230");
    }
    if (settings_->publish_velsensorsetup == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<VelSensorSetupMsg>("4244");
    }
    if (settings_->publish_exteventinsnavcart == true)
    {
        handlers_.callbackmap_ = 
            handlers_.insert<ExtEventINSNavCartMsg>("4229");
    }
	if (settings_->publish_gpst == true)
	{
		handlers_.callbackmap_ = handlers_.insert<int32_t>("GPST");
	}
    if (settings_->septentrio_receiver_type == "gnss")
    {
        if (settings_->publish_navsatfix == true)
        {
            if (settings_->publish_pvtgeodetic == false || settings_->publish_poscovgeodetic == false)
            {
                node_->log(LogLevel::ERROR, 
                    "For a proper NavSatFix message, please set the publish/pvtgeodetic and the publish/poscovgeodetic ROSaic parameters both to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<NavSatFixMsg>("NavSatFix");
        }
    }
    if (settings_->septentrio_receiver_type == "ins")
    {
        if (settings_->publish_navsatfix == true)
        {
            if (settings_->publish_insnavgeod == false)
            {
                node_->log(LogLevel::ERROR, 
                    "For a proper NavSatFix message, please set the publish/insnavgeod to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<NavSatFixMsg>("INSNavSatFix");
        }
    }
    if (settings_->septentrio_receiver_type == "gnss")
    {
        if (settings_->publish_gpsfix == true)
        {
            if (settings_->publish_pvtgeodetic == false || settings_->publish_poscovgeodetic == false || settings_->publish_velcovgeodetic == false)
            {
                node_->log(LogLevel::ERROR, 
                    "For a proper GPSFix message, please set the publish/pvtgeodetic, publish/poscovgeodetic and publish/velcovgeodetic ROSaic parameters all to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<GPSFixMsg>("GPSFix");
            // The following blocks are never published, yet are needed for the
            // construction of the GPSFix message, hence we have empty callbacks.
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4013"); // ChannelStatus block
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4027"); // MeasEpoch block
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4001"); // DOP block
        }
    }
    if (settings_->septentrio_receiver_type == "ins")
    {
        if (settings_->publish_gpsfix == true)
        {
            if (settings_->publish_insnavgeod == false)
            {
                node_->log(LogLevel::ERROR, 
                        "For a proper GPSFix message, please set the publish/insnavgeod to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<GPSFixMsg>("INSGPSFix");
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4013"); // ChannelStatus block
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4027"); // MeasEpoch block
            handlers_.callbackmap_ =
                handlers_.insert<int32_t>("4001"); // DOP block
        }
    }
    if (settings_->septentrio_receiver_type == "gnss")
    {
        if (settings_->publish_pose == true)
        {
            if (settings_->publish_pvtgeodetic == false || settings_->publish_poscovgeodetic == false ||
                settings_->publish_atteuler == false || settings_->publish_attcoveuler == false)
            {
                node_->log(LogLevel::ERROR, 
                    "For a proper PoseWithCovarianceStamped message, please set the publish/pvtgeodetic, publish/poscovgeodetic, publish_atteuler and publish_attcoveuler ROSaic parameters all to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<PoseWithCovarianceStampedMsg>(
                    "PoseWithCovarianceStamped");
        }
    }
    if (settings_->septentrio_receiver_type == "ins")
    {
        if (settings_->publish_pose == true)
        {
            if (settings_->publish_insnavgeod == false)
            {
                node_->log(LogLevel::ERROR, 
                    "For a proper PoseWithCovarianceStamped message, please set the publish/insnavgeod to true.");
            }
            handlers_.callbackmap_ =
                handlers_.insert<PoseWithCovarianceStampedMsg>(
                    "INSPoseWithCovarianceStamped");
        }
    }
	if (settings_->publish_diagnostics == true)
	{
		handlers_.callbackmap_ =
			handlers_.insert<DiagnosticArrayMsg>(
				"DiagnosticArray");
		handlers_.callbackmap_ =
			handlers_.insert<int32_t>("4014"); // ReceiverStatus block
		handlers_.callbackmap_ =
			handlers_.insert<int32_t>("4082"); // QualityInd block
		handlers_.callbackmap_ =
			handlers_.insert<int32_t>("5902"); // ReceiverSetup block
	}
	// so on and so forth...
    node_->log(LogLevel::DEBUG, "Leaving defineMessages() method");
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::send(std::string cmd)
{
    // It is imperative to hold a lock on the mutex "g_response_mutex" while
    // modifying the variable "g_response_received".
    boost::mutex::scoped_lock lock(g_response_mutex);
    // Determine byte size of cmd and hand over to send() method of manager_
    manager_.get()->send(cmd, cmd.size());
    g_response_condition.wait(lock, []() { return g_response_received; });
    g_response_received = false;
}

template<class NodeT>
bool io_comm_rx::Comm_IO<NodeT>::initializeTCP(std::string host, std::string port)
{
    node_->log(LogLevel::DEBUG, "Calling initializeTCP() method..");
    host_ = host;
    port_ = port;
    // The io_context, of which io_service is a typedef of; it represents your
    // program's link to the operating system's I/O services.
    boost::shared_ptr<boost::asio::io_service> io_service(
        new boost::asio::io_service);
    boost::asio::ip::tcp::resolver::iterator endpoint;

    try
    {
        boost::asio::ip::tcp::resolver resolver(*io_service);
        // Note that tcp::resolver::query takes the host to resolve or the IP as the
        // first parameter and the name of the service (as defined e.g. in
        // /etc/services on Unix hosts) as second parameter. For the latter, one can
        // also use a numeric service identifier (aka port number). In any case, it
        // returns a list of possible endpoints, as there might be several entries
        // for a single host.
        endpoint = resolver.resolve(boost::asio::ip::tcp::resolver::query(
            host, port)); // Resolves query object..
    } catch (std::runtime_error& e)
    {
        throw std::runtime_error("Could not resolve " + host + " on port " + port +
                                 ": " + e.what());
        return false;
    }

    boost::shared_ptr<boost::asio::ip::tcp::socket> socket(
        new boost::asio::ip::tcp::socket(*io_service));

    try
    {
        // The list of endpoints obtained above may contain both IPv4 and IPv6
        // endpoints, so we need to try each of them until we find one that works.
        // This keeps the client program independent of a specific IP version. The
        // boost::asio::connect() function does this for us automatically.
        socket->connect(*endpoint);
    } catch (std::runtime_error& e)
    {
        throw std::runtime_error("Could not connect to " + endpoint->host_name() +
                                 ": " + endpoint->service_name() + ": " + e.what());
        return false;
    }

    node_->log(LogLevel::INFO, "Connected to " + endpoint->host_name() + 
                               ":" + endpoint->service_name() + ".");

    if (manager_)
    {
        node_->log(LogLevel::ERROR, 
            "You have called the InitializeTCP() method though an AsyncManager object is already available! Start all anew..");
        return false;
    }
    setManager(boost::shared_ptr<Manager>(
        new AsyncManager<boost::asio::ip::tcp::socket>(socket, io_service)));
    node_->log(LogLevel::DEBUG, "Leaving initializeTCP() method..");
    return true;
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::initializeSBFFileReading(std::string file_name)
{
    node_->log(LogLevel::DEBUG, "Calling initializeSBFFileReading() method..");
    std::size_t buffer_size = 16384;
    uint8_t* to_be_parsed;
    to_be_parsed = new uint8_t[buffer_size];
    std::ifstream bin_file(file_name, std::ios::binary);
    std::vector<uint8_t> vec_buf;
    if (bin_file.good())
    {
        /* Reads binary data using streambuffer iterators.
        Copies all SBF file content into bin_data. */
        std::vector<uint8_t> v_buf((std::istreambuf_iterator<char>(bin_file)),
                                   (std::istreambuf_iterator<char>()));
        vec_buf = v_buf;
        bin_file.close();
    } else
    {
        throw std::runtime_error("I could not find your file. Or it is corrupted.");
    }
    // The spec now guarantees that vectors store their elements contiguously.
    to_be_parsed = vec_buf.data();
    std::stringstream ss;
    ss << "Opened and copied over from " << file_name;
    node_->log(LogLevel::DEBUG, ss.str());

    while (!stopping_) // Loop will stop if we are done reading the SBF file
    {
        try
        {
            node_->log(LogLevel::DEBUG, 
                "Calling read_callback_() method, with number of bytes to be parsed being " +
                buffer_size);
            handlers_.readCallback(to_be_parsed, buffer_size);
        } catch (std::size_t& parsing_failed_here)
        {
            if (to_be_parsed - vec_buf.data() >= vec_buf.size() * sizeof(uint8_t))
            {
                break;
            }
            to_be_parsed = to_be_parsed + parsing_failed_here;
            node_->log(LogLevel::DEBUG, "Parsing_failed_here is " + parsing_failed_here);
            continue;
        }
        if (to_be_parsed - vec_buf.data() >= vec_buf.size() * sizeof(uint8_t))
        {
            break;
        }
        to_be_parsed = to_be_parsed + buffer_size;
    }
    node_->log(LogLevel::DEBUG, "Leaving initializeSBFFileReading() method..");
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::initializePCAPFileReading(std::string file_name)
{
    node_->log(LogLevel::DEBUG, "Calling initializePCAPFileReading() method..");
    pcapReader::buffer_t vec_buf;
    pcapReader::PcapDevice device(vec_buf);

    if (!device.connect(file_name.c_str()))
    {
        node_->log(LogLevel::ERROR, "Unable to find file or either it is corrupted");
        return;
    }

    node_->log(LogLevel::INFO, "Reading ...");
    while (device.isConnected() && device.read() == pcapReader::READ_SUCCESS)
        ;
    device.disconnect();

    std::size_t buffer_size = pcapReader::PcapDevice::BUFFSIZE;
    uint8_t* to_be_parsed = new uint8_t[buffer_size];
    to_be_parsed = vec_buf.data();

    while (!stopping_) // Loop will stop if we are done reading the SBF file
    {
        try
        {
            node_->log(LogLevel::DEBUG, 
                "Calling read_callback_() method, with number of bytes to be parsed being " +
                buffer_size);
            handlers_.readCallback(to_be_parsed, buffer_size);
        } catch (std::size_t& parsing_failed_here)
        {
            if (to_be_parsed - vec_buf.data() >= vec_buf.size() * sizeof(uint8_t))
            {
                break;
            }
            if (!parsing_failed_here)
                parsing_failed_here = 1;

            to_be_parsed = to_be_parsed + parsing_failed_here;
            node_->log(LogLevel::DEBUG, "Parsing_failed_here is " + parsing_failed_here);
            continue;
        }
        if (to_be_parsed - vec_buf.data() >= vec_buf.size() * sizeof(uint8_t))
        {
            break;
        }
        to_be_parsed = to_be_parsed + buffer_size;
    }
    node_->log(LogLevel::DEBUG, "Leaving initializePCAPFileReading() method..");
}

template<class NodeT>
bool io_comm_rx::Comm_IO<NodeT>::initializeSerial(std::string port, uint32_t baudrate,
                                           std::string flowcontrol)
{
    node_->log(LogLevel::DEBUG, "Calling initializeSerial() method..");
    serial_port_ = port;
    baudrate_ = baudrate;
    // The io_context, of which io_service is a typedef of; it represents your
    // program's link to the operating system's I/O services.
    boost::shared_ptr<boost::asio::io_service> io_service(
        new boost::asio::io_service);
    // To perform I/O operations the program needs an I/O object, here "serial".
    boost::shared_ptr<boost::asio::serial_port> serial(
        new boost::asio::serial_port(*io_service));

    // We attempt the opening of the serial port..
    try
    {
        serial->open(serial_port_);
    } catch (std::runtime_error& e)
    {
        // and return an error message in case it fails.
        throw std::runtime_error("Could not open serial port : " + serial_port_ +
                                 ": " + e.what());
        return false;
    }

    node_->log(LogLevel::INFO, "Opened serial port " + serial_port_);
    node_->log(LogLevel::DEBUG, "Our boost version is " + std::to_string(BOOST_VERSION) + ".");
    if (BOOST_VERSION < 106600) // E.g. for ROS melodic (i.e. Ubuntu 18.04), the
                                // version is 106501, standing for 1.65.1.
    {
        // Workaround to set some options for the port manually,
        // cf. https://github.com/mavlink/mavros/pull/971/files
        // This function native_handle() may be used to obtain
        // the underlying representation of the serial port.
        // Conversion from type native_handle_type to int is done implicitly.
        int fd = serial->native_handle();
        termios tio;
        // Get terminal attribute, follows the syntax
        // int tcgetattr(int fd, struct termios *termios_p);
        tcgetattr(fd, &tio);

        // Hardware flow control settings_->.
        if (flowcontrol == "RTS|CTS")
        {
            tio.c_iflag &= ~(IXOFF | IXON);
            tio.c_cflag |= CRTSCTS;
        } else
        {
            tio.c_iflag &= ~(IXOFF | IXON);
            tio.c_cflag &= ~CRTSCTS;
        }
        // Setting serial port to "raw" mode to prevent EOF exit..
        cfmakeraw(&tio);

        // Commit settings, syntax is
        // int tcsetattr(int fd, int optional_actions, const struct termios
        // *termios_p);
        tcsetattr(fd, TCSANOW, &tio);
    }

    // Set the I/O manager
    if (manager_)
    {
        node_->log(LogLevel::ERROR, 
            "You have called the initializeSerial() method though an AsyncManager object is already available! Start all anew..");
        return false;
    }
    node_->log(LogLevel::DEBUG, "Creating new Async-Manager object..");
    setManager(boost::shared_ptr<Manager>(
        new AsyncManager<boost::asio::serial_port>(serial, io_service)));

    // Setting the baudrate, incrementally..
    node_->log(LogLevel::DEBUG, "Gradually increasing the baudrate to the desired value...");
    boost::asio::serial_port_base::baud_rate current_baudrate;
    node_->log(LogLevel::DEBUG, "Initiated current_baudrate object...");
    try
    {
        serial->get_option(
            current_baudrate); // Note that this sets current_baudrate.value() often
                               // to 115200, since by default, all Rx COM ports,
        // at least for mosaic Rxs, are set to a baudrate of 115200 baud, using 8
        // data-bits, no parity and 1 stop-bit.
    } catch (boost::system::system_error& e)
    {

        node_->log(LogLevel::ERROR, "get_option failed due to " +std::string( e.what()));
        node_->log(LogLevel::INFO, "Additional info about error is " +
                 boost::diagnostic_information(e));
        /*
        boost::system::error_code e_loop;
        do // Caution: Might cause infinite loop..
        {
            serial->get_option(current_baudrate, e_loop);
        } while(e_loop);
        */
        return false;
    }
    // Gradually increase the baudrate to the desired value
    // The desired baudrate can be lower or larger than the
    // current baudrate; the for loop takes care of both scenarios.
    node_->log(LogLevel::DEBUG, "Current baudrate is " + current_baudrate.value());
    for (uint8_t i = 0; i < sizeof(BAUDRATES) / sizeof(BAUDRATES[0]); i++)
    {
        if (current_baudrate.value() == baudrate_)
        {
            break; // Break if the desired baudrate has been reached.
        }
        if (current_baudrate.value() >= BAUDRATES[i] && baudrate_ > BAUDRATES[i])
        {
            continue;
        }
        // Increment until Baudrate[i] matches current_baudrate.
        try
        {
            serial->set_option(
                boost::asio::serial_port_base::baud_rate(BAUDRATES[i]));
        } catch (boost::system::system_error& e)
        {

            node_->log(LogLevel::ERROR, "set_option failed due to " + std::string(e.what()));
            node_->log(LogLevel::INFO, "Additional info about error is " +
                     boost::diagnostic_information(e));
            return false;
        }
        usleep(SET_BAUDRATE_SLEEP_);
        // boost::this_thread::sleep(boost::posix_time::milliseconds(SET_BAUDRATE_SLEEP_*1000));
        // Boost's sleep would yield an error message with exit code -7 the second
        // time it is called, hence we use sleep() or usleep().
        try
        {
            serial->get_option(current_baudrate);
        } catch (boost::system::system_error& e)
        {

            node_->log(LogLevel::ERROR, "get_option failed due to " + std::string(e.what()));
            node_->log(LogLevel::INFO, "Additional info about error is " +
                     boost::diagnostic_information(e));
            /*
            boost::system::error_code e_loop;
            do // Caution: Might cause infinite loop..
            {
                serial->get_option(current_baudrate, e_loop);
            } while(e_loop);
            */
            return false;
        }
        node_->log(LogLevel::DEBUG, "Set ASIO baudrate to " + current_baudrate.value());
    }
    node_->log(LogLevel::INFO, "Set ASIO baudrate to " + std::to_string(current_baudrate.value())  + 
                               ", leaving InitializeSerial() method");
    return true;
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::setManager(const boost::shared_ptr<Manager>& manager)
{
    node_->log(LogLevel::DEBUG, "Called setManager() method");
    if (manager_)
        return;
    manager_ = manager;
    manager_->setCallback(
        boost::bind(&CallbackHandlers::readCallback, &handlers_, _1, _2));
    node_->log(LogLevel::DEBUG, "Leaving setManager() method");
}

template<class NodeT>
void io_comm_rx::Comm_IO<NodeT>::resetSerial(std::string port)
{
    serial_port_ = port;
    boost::shared_ptr<boost::asio::io_service> io_service(
        new boost::asio::io_service);
    boost::shared_ptr<boost::asio::serial_port> serial(
        new boost::asio::serial_port(*io_service));

    // Try to open serial port
    try
    {
        serial->open(serial_port_);
    } catch (std::runtime_error& e)
    {
        throw std::runtime_error("Could not open serial port :" + serial_port_ +
                                 " " + e.what());
    }

    node_->log(LogLevel::INFO, "Reset serial port " + serial_port_);

    // Sets the I/O worker
    if (manager_)
        return;
    setManager(boost::shared_ptr<Manager>(
        new AsyncManager<boost::asio::serial_port>(serial, io_service)));

    // Set the baudrate
    serial->set_option(boost::asio::serial_port_base::baud_rate(baudrate_));
}
