/**
 * \file AppMgrCore.cpp
 * \brief App manager core functionality
 * \author vsalo
 */

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#include "AppMgr/AppMgrCore.h"
#include "AppMgr/AppMgrRegistry.h"
#include "AppMgr/AppPolicy.h"
#include "AppMgr/RegistryItem.h"
#include "AppMgr/AppMgrCoreQueues.h"
#include "AppMgr/SubscribeButton.h"
#include "AppMgr/HMIHandler.h"
#include "AppMgr/MobileHandler.h"
#include "AppMgr/ConnectionHandler.h"
#include "JSONHandler/ALRPCMessage.h"
#include "JSONHandler/ALRPCRequest.h"
#include "JSONHandler/ALRPCResponse.h"
#include "JSONHandler/ALRPCNotification.h"
#include "JSONHandler/ALRPCObjects/V1/Marshaller.h"
#include "JSONHandler/ALRPCObjects/V2/Marshaller.h"
#include "JSONHandler/JSONHandler.h"
#include "JSONHandler/JSONRPC2Handler.h"
#include "JSONHandler/RPC2Objects/Marshaller.h"
#include "JSONHandler/RPC2Command.h"
#include "JSONHandler/RPC2Request.h"
#include "JSONHandler/RPC2Response.h"
#include "JSONHandler/RPC2Notification.h"
#include "JSONHandler/ALRPCObjects/V2/AppType.h"
#include "Utils/ClientSocket.h"
#include "Utils/SocketException.h"
#include <sys/socket.h>
#include "LoggerHelper.hpp"
#include <iostream>
#include <fstream>
#include <cstddef>

namespace {
    unsigned long int getAvailableSpace() {
        char currentAppPath[256];
        memset((void*)currentAppPath, 0, 256);
        getcwd(currentAppPath, 255);

        struct statvfs fsInfo;
        memset((void*)&fsInfo, 0, sizeof(fsInfo));
        statvfs(currentAppPath, &fsInfo);
        return fsInfo.f_bsize * fsInfo.f_bfree;
    }

    template<typename Response, typename Result>
    void sendResponse(int responseId, Result result)
    {
        Response* response = new Response;
        response->setId(responseId);
        response->setResult(result);
        NsAppManager::HMIHandler::getInstance().sendResponse(response);
    }

    struct thread_data
    {
        int  timeout;
        std::string url;
        NsAppManager::SyncPManager::PData pdata;
    };

    void *SendPData(void *data)
    {
        log4cplus::Logger logger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("AppMgrCore"));
        LOG4CPLUS_INFO_EXT(logger, " Started data sending thread");
        struct thread_data* my_data = (struct thread_data*) data;
        int timeout = my_data->timeout;
        std::string url = my_data->url;
        NsAppManager::SyncPManager::PData pData = my_data->pdata;
        LOG4CPLUS_INFO_EXT(logger, " Sending params: url " << url << " timeout " << timeout << " data of " << pData.size() << " lines");
        sleep(timeout);
        int port = 80;
        size_t pos = url.find(":");
        if(pos != std::string::npos)
        {
            std::string strPort = url.substr(pos+1);
            if(!strPort.empty())
            {
                port = atoi(strPort.c_str());
            }
        }
        std::string host = url.substr(0, pos);
        LOG4CPLUS_INFO_EXT(logger, " Sending at " << host << " port " << port);
        ClientSocket client_socket( host, port );
  //      std::string reply;
        for(NsAppManager::SyncPManager::PData::iterator it = pData.begin(); it != pData.end(); it++)
        {
            LOG4CPLUS_INFO_EXT(logger, " Sending data " << *it);
            client_socket << *it;
  //          client_socket >> reply;
        }
        LOG4CPLUS_INFO_EXT(logger, " All data sent to host " << host << " port " << port);
        pthread_exit(NULL);
    }
}

namespace NsAppManager
{
    log4cplus::Logger AppMgrCore::mLogger = log4cplus::Logger::getInstance(LOG4CPLUS_TEXT("AppMgrCore"));

    /**
     * \brief Returning class instance
     * \return class instance
     */
    AppMgrCore& AppMgrCore::getInstance( )
    {
        static AppMgrCore appMgr;
        return appMgr;
    }

    /**
     * \brief Default class constructor
     */
    AppMgrCore::AppMgrCore()
        :mQueueRPCAppLinkObjectsIncoming(new AppMgrCoreQueue<Message>(&AppMgrCore::handleMobileRPCMessage, this))
        ,mQueueRPCBusObjectsIncoming(new AppMgrCoreQueue<NsRPC2Communication::RPC2Command*>(&AppMgrCore::handleBusRPCMessageIncoming, this))
        ,mDriverDistractionV1(0)
        ,mDriverDistractionV2(0)
    {
        LOG4CPLUS_INFO_EXT(mLogger, " AppMgrCore constructed!");
    }

    /**
     * \brief Copy constructor
     */
    AppMgrCore::AppMgrCore(const AppMgrCore &)
        :mQueueRPCAppLinkObjectsIncoming(0)
        ,mQueueRPCBusObjectsIncoming(0)
        ,mDriverDistractionV1(0)
        ,mDriverDistractionV2(0)
    {
    }

    /**
     * \brief Default class destructor
     */
    AppMgrCore::~AppMgrCore()
    {
        if(mQueueRPCAppLinkObjectsIncoming)
            delete mQueueRPCAppLinkObjectsIncoming;
        if(mQueueRPCBusObjectsIncoming)
            delete mQueueRPCBusObjectsIncoming;
        if(mDriverDistractionV1)
            delete mDriverDistractionV1;
        if(mDriverDistractionV2)
            delete mDriverDistractionV2;

        LOG4CPLUS_INFO_EXT(mLogger, " AppMgrCore destructed!");
    }

    /**
     * \brief push mobile RPC message to a queue
     * \param message a message to be pushed
     * \param connectionID id of a connection associated with application that sent the message
     * \param sessionID an id of a session associated with the application which pushes a message
     */
    void AppMgrCore::pushMobileRPCMessage( NsAppLinkRPC::ALRPCMessage * message, int appId )
    {
        LOG4CPLUS_INFO_EXT(mLogger, " Pushing mobile RPC message " << message->getMethodId() << " for application id " << appId << "...");
        if(!message)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "Nothing to push! A null-ptr occured!");
            return;
        }

        mQueueRPCAppLinkObjectsIncoming->pushMessage(Message(message, appId));

        LOG4CPLUS_INFO_EXT(mLogger, " Pushed mobile RPC message " << message->getMethodId() << " for application id " << appId);
    }

    /**
     * \brief push HMI RPC2 message to a queue
     * \param message a message to be pushed
     */
    void AppMgrCore::pushRPC2CommunicationMessage( NsRPC2Communication::RPC2Command * message )
    {
        LOG4CPLUS_INFO_EXT(mLogger, " Returning a message " << message->getMethod() << " from HMI...");
        if(!message)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "Nothing to push! A null-ptr occured!");
            return;
        }

        mQueueRPCBusObjectsIncoming->pushMessage(message);

        LOG4CPLUS_INFO_EXT(mLogger, " Returned a message " << message->getMethod() << " from HMI");
    }

    /**
     * \brief method to execute threads.
     */
    void AppMgrCore::executeThreads()
    {
        LOG4CPLUS_INFO_EXT(mLogger, " Threads are being started!");

        mQueueRPCAppLinkObjectsIncoming->executeThreads();
        mQueueRPCBusObjectsIncoming->executeThreads();

        LOG4CPLUS_INFO_EXT(mLogger, " Threads have been started!");
    }

    /**
     * \brief mobile RPC message handler
     * \param mesage a message to be handled
     * \param pThis a pointer to AppMgrCore class instance
     */
    void AppMgrCore::handleMobileRPCMessage(Message message , void *pThis)
    {
        int sessionKey = message.second;

        NsAppLinkRPC::ALRPCMessage* mobileMsg = message.first;
        if(!mobileMsg)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, " No message associated with the session key " << sessionKey << " !");
            return;
        }
        LOG4CPLUS_INFO_EXT(mLogger, " A mobile RPC message " << mobileMsg->getMethodId() << " has been received for the session key " << sessionKey << " !");
        if(!pThis)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, " pThis should point to an instance of AppMgrCore class");
            return;
        }
        AppMgrCore* core = (AppMgrCore*)pThis;
        const unsigned int& protocolVersion = mobileMsg->getProtocolVersion();
        const NsConnectionHandler::tDeviceHandle& currentDeviceHandle = core->mDeviceHandler.findDeviceAssignedToSession(sessionKey);
        const NsConnectionHandler::CDevice* currentDevice = core->mDeviceList.findDeviceByHandle(currentDeviceHandle);
        if(!currentDevice)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, " Cannot retreive current device name for the message with session key " << sessionKey << " !");
            return;
        }
        const std::string& currentDeviceName = currentDevice->getUserFriendlyName();

        LOG4CPLUS_INFO_EXT(mLogger, "Message received is from protocol " << protocolVersion);
        if ( 1 == mobileMsg->getProtocolVersion() )
        {
            switch(mobileMsg->getMethodId())
            {
                case NsAppLinkRPC::Marshaller::METHOD_REGISTERAPPINTERFACE_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A RegisterAppInterface request has been invoked");

                    NsAppLinkRPC::RegisterAppInterface_request * object = (NsAppLinkRPC::RegisterAppInterface_request*)mobileMsg;
                    NsAppLinkRPC::RegisterAppInterface_response* response = new NsAppLinkRPC::RegisterAppInterface_response();
                    const std::string& appName = object->get_appName();

                    if(AppMgrRegistry::getInstance().getItem(sessionKey))
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " Application " << appName << " is already registered!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_REGISTERED_ALREADY);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    Application_v1* app = (Application_v1*)core->getApplicationFromItemCheckNotNull(core->registerApplication( object, sessionKey ));
                    response->setCorrelationID(object->getCorrelationID());
                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " Application " << appName << " hasn't been registered!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPC::OnHMIStatus* status = new NsAppLinkRPC::OnHMIStatus();
                    status->set_hmiLevel(app->getApplicationHMIStatusLevel());
                    status->set_audioStreamingState(app->getApplicationAudioStreamingState());
                    status->set_systemContext(app->getSystemContext());
                    MobileHandler::getInstance().sendRPCMessage(status, sessionKey);
                    LOG4CPLUS_INFO_EXT(mLogger, " An OnHMIStatus notification for the app "  << app->getName()
                        << " connection/session key " << app->getAppID()
                        << " gets sent to a mobile side... ");

                    response->set_buttonCapabilities(core->mButtonCapabilitiesV1.get());
                    response->set_displayCapabilities(core->mDisplayCapabilitiesV1);
                    response->set_hmiZoneCapabilities(core->mHmiZoneCapabilitiesV1.get());
                    response->set_speechCapabilities(core->mSpeechCapabilitiesV1.get());
                    response->set_vrCapabilities(core->mVrCapabilitiesV1.get());
                    response->set_language(core->mUiLanguageV1);
                    response->set_syncMsgVersion(app->getSyncMsgVersion());
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPC::Result::SUCCESS);

                    LOG4CPLUS_INFO_EXT(mLogger, " A RegisterAppInterface response for the app "  << app->getName()
                        << " connection/session key " << app->getAppID()
                        << " gets sent to a mobile side... ");
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);

                    NsRPC2Communication::AppLinkCore::OnAppRegistered* appRegistered = new NsRPC2Communication::AppLinkCore::OnAppRegistered();
                    appRegistered->set_appName(app->getName());
                    appRegistered->set_appId(app->getAppID());
                    appRegistered->set_isMediaApplication(app->getIsMediaApplication());
                    appRegistered->set_languageDesired(app->getLanguageDesired());
                    appRegistered->set_vrSynonym(app->getVrSynonyms());
                    appRegistered->set_deviceName(currentDeviceName);
                    appRegistered->set_versionNumber(1);
                    HMIHandler::getInstance().sendNotification(appRegistered);
                    LOG4CPLUS_INFO_EXT(mLogger, " An AppLinkCore::OnAppRegistered notofocation for the app " << app->getName()
                        << " application id " << app->getAppID()
                        << " gets sent to an HMI side... ");
                    LOG4CPLUS_INFO_EXT(mLogger, " A RegisterAppInterface request was successful: registered an app " << app->getName()
                        << " application id " << app->getAppID());
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_UNREGISTERAPPINTERFACE_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An UnregisterAppInterface request has been invoked");

                    NsAppLinkRPC::UnregisterAppInterface_request * object = (NsAppLinkRPC::UnregisterAppInterface_request*)mobileMsg;
                    Application* app = core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    NsAppLinkRPC::UnregisterAppInterface_response* response = new NsAppLinkRPC::UnregisterAppInterface_response();
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey 
                            << " hasn't been associated with any application!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    std::string appName = app->getName();

                    core->removeAppFromHmi(app, sessionKey);
                    core->unregisterApplication( sessionKey );

                    response->setCorrelationID(object->getCorrelationID());
                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);

                    NsAppLinkRPC::OnAppInterfaceUnregistered* msgUnregistered = new NsAppLinkRPC::OnAppInterfaceUnregistered();
                    msgUnregistered->set_reason(NsAppLinkRPC::AppInterfaceUnregisteredReason(NsAppLinkRPC::AppInterfaceUnregisteredReason::USER_EXIT));
                    MobileHandler::getInstance().sendRPCMessage(msgUnregistered, sessionKey);
                    NsRPC2Communication::AppLinkCore::OnAppUnregistered* appUnregistered = new NsRPC2Communication::AppLinkCore::OnAppUnregistered();
                    appUnregistered->set_appName(appName);
                    appUnregistered->set_appId(app->getAppID());
                    appUnregistered->set_reason(NsAppLinkRPC::AppInterfaceUnregisteredReason(NsAppLinkRPC::AppInterfaceUnregisteredReason::USER_EXIT));
                    HMIHandler::getInstance().sendNotification(appUnregistered);

                    LOG4CPLUS_INFO_EXT(mLogger, " An application " << appName << " has been unregistered successfully ");
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SUBSCRIBEBUTTON_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SubscribeButton request has been invoked");

                    /* RegistryItem* registeredApp = AppMgrRegistry::getInstance().getItem(sessionID);
                ButtonParams* params = new ButtonParams();
                params->mMessage = message;
                IAppCommand* command = new SubscribeButtonCmd(registeredApp, params);
                command->execute();
                delete command; */

                    NsAppLinkRPC::SubscribeButton_request * object = (NsAppLinkRPC::SubscribeButton_request*)mobileMsg;
                    NsAppLinkRPC::SubscribeButton_response* response = new NsAppLinkRPC::SubscribeButton_response();
                    RegistryItem* item = AppMgrRegistry::getInstance().getItem(sessionKey);
                    if(!item)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    Application_v1* app = (Application_v1*)item->getApplication();
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with the registry item with session key " << sessionKey );
                        break;
                    }
                    if(NsAppLinkRPC::HMILevel::HMI_NONE == app->getApplicationHMIStatusLevel())
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "An application " << app->getName() << " with session key " << sessionKey << " has not been activated yet!" );
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::REJECTED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsAppLinkRPCV2::ButtonName btnName;
                    btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)object->get_buttonName().get());
                    core->mButtonsMapping.addButton( btnName, item );

                    response->setCorrelationID(object->getCorrelationID());
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_UNSUBSCRIBEBUTTON_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An UnsubscribeButton request has been invoked");
                    NsAppLinkRPC::UnsubscribeButton_request * object = (NsAppLinkRPC::UnsubscribeButton_request*)mobileMsg;
                    NsAppLinkRPC::UnsubscribeButton_response* response = new NsAppLinkRPC::UnsubscribeButton_response();
                    Application_v1* app = (Application_v1*)core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with the registry item with session key " << sessionKey );
                        break;
                    }
                    if(NsAppLinkRPC::HMILevel::HMI_NONE == app->getApplicationHMIStatusLevel())
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "An application " << app->getName() << " with session key " << sessionKey << " has not been activated yet!" );
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::REJECTED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsAppLinkRPCV2::ButtonName btnName;
                    btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)object->get_buttonName().get());
                    core->mButtonsMapping.removeButton( btnName );
                    response->setCorrelationID(object->getCorrelationID());
                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SHOW_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A Show request has been invoked");
                    LOG4CPLUS_INFO_EXT(mLogger, "message " << mobileMsg->getMethodId() );
                    NsAppLinkRPC::Show_request* object = (NsAppLinkRPC::Show_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with the registry item with session key " << sessionKey );
                        break;
                    }
                    if(NsAppLinkRPC::HMILevel::HMI_NONE == app->getApplicationHMIStatusLevel())
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "An application " << app->getName() << " with session key " << sessionKey << " has not been activated yet!" );
                        NsAppLinkRPC::Show_response* response = new NsAppLinkRPC::Show_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::REJECTED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::Show* showRPC2Request = new NsRPC2Communication::UI::Show();
                    showRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    LOG4CPLUS_INFO_EXT(mLogger, "showrpc2request created");
                    if(object->get_mainField1())
                    {
                        showRPC2Request->set_mainField1(*object->get_mainField1());
                    }
                    LOG4CPLUS_INFO_EXT(mLogger, "setMainField1 was called");
                    if(object->get_mainField2())
                    {
                        showRPC2Request->set_mainField2(*object->get_mainField2());
                    }
                    if(object->get_mediaClock())
                    {
                        showRPC2Request->set_mediaClock(*object->get_mediaClock());
                    }
                    if(object->get_mediaTrack())
                    {
                        showRPC2Request->set_mediaTrack(*object->get_mediaTrack());
                    }
                    if(object->get_statusBar())
                    {
                        showRPC2Request->set_statusBar(*object->get_statusBar());
                    }
                    if(object->get_alignment())
                    {
                        showRPC2Request->set_alignment(*object->get_alignment());
                    }
                    showRPC2Request->set_appId(sessionKey);
                    LOG4CPLUS_INFO_EXT(mLogger, "Show request almost handled" );
                    core->mMessageMapping.addMessage(showRPC2Request->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(showRPC2Request);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SPEAK_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A Speak request has been invoked");
                    NsAppLinkRPC::Speak_request* object = (NsAppLinkRPC::Speak_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with the registry item with session key " << sessionKey );
                        break;
                    }
                    if(NsAppLinkRPC::HMILevel::HMI_NONE == app->getApplicationHMIStatusLevel())
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "An application " << app->getName() << " with session key " << sessionKey << " has not been activated yet!" );
                        NsAppLinkRPC::Speak_response* response = new NsAppLinkRPC::Speak_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::REJECTED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::TTS::Speak* speakRPC2Request = new NsRPC2Communication::TTS::Speak();
                    speakRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    speakRPC2Request->set_ttsChunks(object->get_ttsChunks());
                    speakRPC2Request->set_appId(sessionKey);
                    core->mMessageMapping.addMessage(speakRPC2Request->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(speakRPC2Request);
                    NsAppLinkRPC::Speak_response * mobileResponse = new NsAppLinkRPC::Speak_response;
                    mobileResponse->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    mobileResponse->set_success(true);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SETGLOBALPROPERTIES_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SetGlobalProperties request has been invoked");
                    NsAppLinkRPC::SetGlobalProperties_request* object = (NsAppLinkRPC::SetGlobalProperties_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with the registry item with session key " << sessionKey );
                        break;
                    }
                    if(NsAppLinkRPC::HMILevel::HMI_NONE == app->getApplicationHMIStatusLevel())
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "An application " << app->getName() << " with session key " << sessionKey << " has not been activated yet!" );
                        NsAppLinkRPC::SetGlobalProperties_response* response = new NsAppLinkRPC::SetGlobalProperties_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::REJECTED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::SetGlobalProperties* setGPRPC2Request = new NsRPC2Communication::UI::SetGlobalProperties();
                    setGPRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(setGPRPC2Request->getId(), sessionKey);
                    if(object->get_helpPrompt())
                    {
                        setGPRPC2Request->set_helpPrompt(*object->get_helpPrompt());
                    }

                    if(object->get_timeoutPrompt())
                    {
                        setGPRPC2Request->set_timeoutPrompt(*object->get_timeoutPrompt());
                    }

                    setGPRPC2Request->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(setGPRPC2Request);
                    NsAppLinkRPC::SetGlobalProperties_response * mobileResponse = new NsAppLinkRPC::SetGlobalProperties_response;
                    mobileResponse->set_success(true);
                    mobileResponse->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_RESETGLOBALPROPERTIES_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A ResetGlobalProperties request has been invoked");
                    NsAppLinkRPC::ResetGlobalProperties_request* object = (NsAppLinkRPC::ResetGlobalProperties_request*)mobileMsg;
                    NsRPC2Communication::UI::ResetGlobalProperties* resetGPRPC2Request = new NsRPC2Communication::UI::ResetGlobalProperties();
                    resetGPRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(resetGPRPC2Request->getId(), sessionKey);
                    resetGPRPC2Request->set_properties(object->get_properties());
                    resetGPRPC2Request->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(resetGPRPC2Request);
                    NsAppLinkRPC::ResetGlobalProperties_response * mobileResponse = new NsAppLinkRPC::ResetGlobalProperties_response;
                    mobileResponse->set_success(true);
                    mobileResponse->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ALERT_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An Alert request has been invoked");
                    NsAppLinkRPC::Alert_request* object = (NsAppLinkRPC::Alert_request*)mobileMsg;
                    NsRPC2Communication::UI::Alert* alert = new NsRPC2Communication::UI::Alert();
                    alert->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(alert->getId(), sessionKey);
                    if(object->get_alertText1())
                    {
                        alert->set_AlertText1(*object->get_alertText1());
                    }
                    if(object->get_alertText2())
                    {
                        alert->set_AlertText2(*object->get_alertText2());
                    }
                    if(object->get_duration())
                    {
                        alert->set_duration(*object->get_duration());
                    }
                    if(object->get_playTone())
                    {
                        alert->set_playTone(*object->get_playTone());
                    }
                    alert->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(alert);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ONBUTTONPRESS:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "OnButtonPress Notification has been received.");
                    MobileHandler::getInstance().sendRPCMessage(mobileMsg, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ONCOMMAND:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "OnCommand Notification has been received.");
                    MobileHandler::getInstance().sendRPCMessage(mobileMsg, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ADDCOMMAND_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand request has been invoked");

                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::AddCommand_response* response = new NsAppLinkRPC::AddCommand_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsAppLinkRPC::AddCommand_request* object = (NsAppLinkRPC::AddCommand_request*)mobileMsg;

                    const unsigned int& cmdId = object->get_cmdID();

                    if(object->get_menuParams())
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand UI request has been invoked");
                        NsRPC2Communication::UI::AddCommand * addCmd = new NsRPC2Communication::UI::AddCommand();
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        CommandType cmdType = CommandType::UI;
                        const NsAppLinkRPC::MenuParams* menuParams = object->get_menuParams();
                        addCmd->set_menuParams(*menuParams);
                        addCmd->set_cmdId(cmdId);
                        addCmd->set_appId(app->getAppID());
                        if(menuParams->get_parentID())
                        {
                            const unsigned int& menuId = *menuParams->get_parentID();
                            app->addMenuCommand(cmdId, menuId);
                        }
                        core->mMessageMapping.addMessage(addCmd->getId(), sessionKey);

                        CommandParams params;
                        params.menuParams = menuParams;
                        app->addCommand(cmdId, cmdType, params);
                        app->incrementUnrespondedRequestCount(cmdId);
                        core->mRequestMapping.addMessage(addCmd->getId(), cmdId);
                        HMIHandler::getInstance().sendRequest(addCmd);

                    }
                    if(object->get_vrCommands())
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand VR request has been invoked");
                        NsRPC2Communication::VR::AddCommand * addCmd = new NsRPC2Communication::VR::AddCommand();
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        CommandType cmdType = CommandType::VR;
                        addCmd->set_vrCommands(*object->get_vrCommands());
                        addCmd->set_cmdId(cmdId);
                        addCmd->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(addCmd->getId(), sessionKey);
                        CommandParams params;
                        params.vrCommands = object->get_vrCommands();
                        app->addCommand(cmdId, cmdType, params);
                        app->incrementUnrespondedRequestCount(cmdId);
                        core->mRequestMapping.addMessage(addCmd->getId(), cmdId);
                        HMIHandler::getInstance().sendRequest(addCmd);
                    }

                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_DELETECOMMAND_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand request has been invoked");
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " Application id " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::DeleteCommand_response* response = new NsAppLinkRPC::DeleteCommand_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPC::DeleteCommand_request* object = (NsAppLinkRPC::DeleteCommand_request*)mobileMsg;

                    CommandTypes cmdTypes = app->getCommandTypes(object->get_cmdID());
                    if(cmdTypes.empty())
                    {
                        NsAppLinkRPC::DeleteCommand_response* response = new NsAppLinkRPC::DeleteCommand_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& cmdId = object->get_cmdID();
                    for(CommandTypes::iterator it = cmdTypes.begin(); it != cmdTypes.end(); it++)
                    {
                        CommandType cmdType = *it;
                        if(cmdType == CommandType::UI)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand UI request has been invoked");
                            NsRPC2Communication::UI::DeleteCommand* deleteCmd = new NsRPC2Communication::UI::DeleteCommand();
                            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                            deleteCmd->set_appId(app->getAppID());
                            core->mMessageMapping.addMessage(deleteCmd->getId(), sessionKey);
                            deleteCmd->set_cmdId(cmdId);
                            app->removeCommand(cmdId, cmdType);
                            app->incrementUnrespondedRequestCount(cmdId);
                            app->removeMenuCommand(cmdId);
                            core->mRequestMapping.addMessage(deleteCmd->getId(), cmdId);
                            HMIHandler::getInstance().sendRequest(deleteCmd);
                        }
                        else if(cmdType == CommandType::VR)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand VR request has been invoked");
                            NsRPC2Communication::VR::DeleteCommand* deleteCmd = new NsRPC2Communication::VR::DeleteCommand();
                            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                            core->mMessageMapping.addMessage(deleteCmd->getId(), sessionKey);
                            deleteCmd->set_cmdId(cmdId);
                            deleteCmd->set_appId(app->getAppID());
                            app->removeCommand(cmdId, cmdType);
                            app->incrementUnrespondedRequestCount(cmdId);
                            core->mRequestMapping.addMessage(deleteCmd->getId(), cmdId);
                            HMIHandler::getInstance().sendRequest(deleteCmd);
                        }
                    }
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ADDSUBMENU_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An AddSubmenu request has been invoked");
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::AddSubMenu_response* response = new NsAppLinkRPC::AddSubMenu_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPC::AddSubMenu_request* object = (NsAppLinkRPC::AddSubMenu_request*)mobileMsg;
                    NsRPC2Communication::UI::AddSubMenu* addSubMenu = new NsRPC2Communication::UI::AddSubMenu();
                    addSubMenu->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(addSubMenu->getId(), sessionKey);
                    addSubMenu->set_menuId(object->get_menuID());
                    addSubMenu->set_menuName(object->get_menuName());
                    if(object->get_position())
                    {
                        addSubMenu->set_position(*object->get_position());
                    }
                    addSubMenu->set_appId(app->getAppID());
                    app->addMenu(object->get_menuID(), object->get_menuName(), object->get_position());
                    HMIHandler::getInstance().sendRequest(addSubMenu);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_DELETESUBMENU_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteSubmenu request has been invoked");
                    NsAppLinkRPC::DeleteSubMenu_request* object = (NsAppLinkRPC::DeleteSubMenu_request*)mobileMsg;
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::DeleteSubMenu_response* response = new NsAppLinkRPC::DeleteSubMenu_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& menuId = object->get_menuID();
                    if(!app->findMenu(menuId))
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " menuId " << menuId
                                            << " hasn't been associated with the application " << app->getName() << " id " << app->getAppID() << " !");
                        NsAppLinkRPC::DeleteSubMenu_response* response = new NsAppLinkRPC::DeleteSubMenu_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::DeleteSubMenu* delSubMenu = new NsRPC2Communication::UI::DeleteSubMenu();
                    delSubMenu->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(delSubMenu->getId(), sessionKey);
                    delSubMenu->set_menuId(menuId);
                    delSubMenu->set_appId(app->getAppID());
                    const MenuCommands& menuCommands = app->findMenuCommands(menuId);
                    LOG4CPLUS_INFO_EXT(mLogger, " A given menu has " << menuCommands.size() << " UI commands - about to delete 'em!");
                    for(MenuCommands::const_iterator it = menuCommands.begin(); it != menuCommands.end(); it++)
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " Deleting command with id " << *it);
                        NsRPC2Communication::UI::DeleteCommand* delUiCmd = new NsRPC2Communication::UI::DeleteCommand();
                        delUiCmd->set_cmdId(*it);
                        delUiCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        delUiCmd->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(delUiCmd->getId(), sessionKey);
                        core->mRequestMapping.addMessage(delUiCmd->getId(), *it);
                        HMIHandler::getInstance().sendRequest(delUiCmd);
                        const CommandTypes& types = app->getCommandTypes(*it);
                        for(CommandTypes::const_iterator it2 = types.begin(); it2 != types.end(); it2++)
                        {
                            const CommandType& type = *it2;
                            if(type == CommandType::VR)
                            {
                                LOG4CPLUS_INFO_EXT(mLogger, " A given command id " << *it << " has VR counterpart attached to: deleting it also!");
                                NsRPC2Communication::VR::DeleteCommand* delVrCmd = new NsRPC2Communication::VR::DeleteCommand();
                                delVrCmd->set_cmdId(*it);
                                delVrCmd->set_appId(app->getAppID());
                                core->mMessageMapping.addMessage(delVrCmd->getId(), sessionKey);
                                core->mRequestMapping.addMessage(delVrCmd->getId(), *it);
                                app->removeCommand(*it, CommandType::VR);
                                HMIHandler::getInstance().sendRequest(delVrCmd);
                            }
                        }
                        app->removeCommand(*it, CommandType::UI);
                        app->removeMenuCommand(*it);
                    }
                    app->removeMenu(menuId);
                    HMIHandler::getInstance().sendRequest(delSubMenu);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_CREATEINTERACTIONCHOICESET_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A CreateInteractionChoiceSet request has been invoked");
                    NsAppLinkRPC::CreateInteractionChoiceSet_request* object = (NsAppLinkRPC::CreateInteractionChoiceSet_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::CreateInteractionChoiceSet_response* response = new NsAppLinkRPC::CreateInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::CreateInteractionChoiceSet* createInteractionChoiceSet = new NsRPC2Communication::UI::CreateInteractionChoiceSet();
                    createInteractionChoiceSet->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(createInteractionChoiceSet->getId(), sessionKey);
                    createInteractionChoiceSet->set_choiceSet(object->get_choiceSet());
                    createInteractionChoiceSet->set_interactionChoiceSetID(object->get_interactionChoiceSetID());
                    createInteractionChoiceSet->set_appId(app->getAppID());
                    app->addChoiceSet(object->get_interactionChoiceSetID(), object->get_choiceSet());
                    HMIHandler::getInstance().sendRequest(createInteractionChoiceSet);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_DELETEINTERACTIONCHOICESET_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteInteractionChoiceSet request has been invoked");
                    NsAppLinkRPC::DeleteInteractionChoiceSet_request* object = (NsAppLinkRPC::DeleteInteractionChoiceSet_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPC::DeleteInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& choiceSetId = object->get_interactionChoiceSetID();
                    const ChoiceSetV1* choiceSetFound = app->findChoiceSet(choiceSetId);
                    if(!choiceSetFound)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " a choice set " << choiceSetId
                                            << " hasn't been registered within the application " << app->getName() << " id" << app->getAppID() << " !");
                        NsAppLinkRPC::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPC::DeleteInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        return;
                    }

                    NsRPC2Communication::UI::DeleteInteractionChoiceSet* deleteInteractionChoiceSet = new NsRPC2Communication::UI::DeleteInteractionChoiceSet();
                    deleteInteractionChoiceSet->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(deleteInteractionChoiceSet->getId(), sessionKey);
                    deleteInteractionChoiceSet->set_interactionChoiceSetID(object->get_interactionChoiceSetID());
                    deleteInteractionChoiceSet->set_appId(app->getAppID());
                    app->removeChoiceSet(object->get_interactionChoiceSetID());
                    HMIHandler::getInstance().sendRequest(deleteInteractionChoiceSet);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_PERFORMINTERACTION_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A PerformInteraction request has been invoked");
                    NsAppLinkRPC::PerformInteraction_request* object = (NsAppLinkRPC::PerformInteraction_request*)mobileMsg;
                    Application_v1* app = (Application_v1*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPC::PerformInteraction_response* response = new NsAppLinkRPC::PerformInteraction_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const std::vector<unsigned int>& choiceSets = object->get_interactionChoiceSetIDList();
                    for(std::vector<unsigned int>::const_iterator it = choiceSets.begin(); it != choiceSets.end(); it++)
                    {
                        const unsigned int& choiceSetId = *it;
                        const ChoiceSetV1* choiceSetFound = app->findChoiceSet(choiceSetId);
                        if(!choiceSetFound)
                        {
                            LOG4CPLUS_ERROR_EXT(mLogger, " a choice set " << choiceSetId
                                                << " hasn't been registered within the application " << app->getName() << " id" << app->getAppID() << " !");
                            NsAppLinkRPC::PerformInteraction_response* response = new NsAppLinkRPC::PerformInteraction_response;
                            response->set_success(false);
                            response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                            MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                            return;
                        }
                    }
                    NsRPC2Communication::UI::PerformInteraction* performInteraction = new NsRPC2Communication::UI::PerformInteraction();
                    performInteraction->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    performInteraction->set_appId(sessionKey);
                    performInteraction->set_interactionChoiceSetIDList(choiceSets);
                    core->mMessageMapping.addMessage(performInteraction->getId(), sessionKey);
                    if(object->get_helpPrompt())
                    {
                        performInteraction->set_helpPrompt(*object->get_helpPrompt());
                    }
                    performInteraction->set_initialPrompt(object->get_initialPrompt());
                    performInteraction->set_initialText(object->get_initialText());
                    performInteraction->set_interactionMode(object->get_interactionMode());
                    if(object->get_timeout())
                    {
                        performInteraction->set_timeout(*object->get_timeout());
                    }
                    if(object->get_timeoutPrompt())
                    {
                        performInteraction->set_timeoutPrompt(*object->get_timeoutPrompt());
                    }
                    HMIHandler::getInstance().sendRequest(performInteraction);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SETMEDIACLOCKTIMER_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SetMediaClockTimer request has been invoked");
                    NsRPC2Communication::UI::SetMediaClockTimer* setTimer = new NsRPC2Communication::UI::SetMediaClockTimer();
                    setTimer->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    setTimer->set_appId(sessionKey);
                    core->mMessageMapping.addMessage(setTimer->getId(), sessionKey);

                    NsAppLinkRPC::SetMediaClockTimer_request* object = (NsAppLinkRPC::SetMediaClockTimer_request*)mobileMsg;
                    if(object->get_startTime())
                    {
                        setTimer->set_startTime(*object->get_startTime());
                    }
                    setTimer->set_updateMode(object->get_updateMode());
                    HMIHandler::getInstance().sendRequest(setTimer);

                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_ENCODEDSYNCPDATA_REQUEST:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An EncodedSyncPData request has been invoked");

                    NsAppLinkRPC::EncodedSyncPData_request* object = (NsAppLinkRPC::EncodedSyncPData_request*)mobileMsg;
                    NsAppLinkRPC::EncodedSyncPData_response* response = new NsAppLinkRPC::EncodedSyncPData_response;
                    if(object->get_data())
                    {
                        Application* app = core->getApplicationFromItemCheckNotNull( AppMgrRegistry::getInstance().getItem(sessionKey) );
                        const std::string& name = app->getName();
                        core->mSyncPManager.setPData(*object->get_data(), name, object->getMethodId());
                        response->set_success(true);
                        response->set_resultCode(NsAppLinkRPC::Result::SUCCESS);
                    }
                    else
                    {
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                    }

                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_SHOW_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_SPEAK_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_SETGLOBALPROPERTIES_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_RESETGLOBALPROPERTIES_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_REGISTERAPPINTERFACE_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_SUBSCRIBEBUTTON_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_UNSUBSCRIBEBUTTON_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_ONAPPINTERFACEUNREGISTERED:
                case NsAppLinkRPC::Marshaller::METHOD_ALERT_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_ADDCOMMAND_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_ADDSUBMENU_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_CREATEINTERACTIONCHOICESET_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_DELETECOMMAND_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_DELETEINTERACTIONCHOICESET_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_DELETESUBMENU_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_ENCODEDSYNCPDATA_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_GENERICRESPONSE_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_PERFORMINTERACTION_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_SETMEDIACLOCKTIMER_RESPONSE:
                case NsAppLinkRPC::Marshaller::METHOD_UNREGISTERAPPINTERFACE_RESPONSE:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A " << mobileMsg->getMethodId() << " response or notification has been invoked");
                    MobileHandler::getInstance().sendRPCMessage(mobileMsg, sessionKey);
                    break;
                }
                case NsAppLinkRPC::Marshaller::METHOD_INVALID:
                default:
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, " An undefined or invalid RPC message " << mobileMsg->getMethodId() << " has been received!");
                    NsAppLinkRPC::GenericResponse_response* response = new NsAppLinkRPC::GenericResponse_response();
                    response->set_success(false);
                    response->set_resultCode(NsAppLinkRPC::Result::INVALID_DATA);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
            }
        }
        else if ( 2 == mobileMsg->getProtocolVersion() )
        {
            LOG4CPLUS_INFO_EXT(mLogger,"Received message of version 2.");
            switch(mobileMsg->getMethodId())
            {
                case NsAppLinkRPCV2::FunctionID::RegisterAppInterfaceID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "Message id is NsAppLinkRPCV2::FunctionID::RegisterAppInterfaceID");
                    NsAppLinkRPCV2::RegisterAppInterface_request * object = (NsAppLinkRPCV2::RegisterAppInterface_request*)mobileMsg;
                    NsAppLinkRPCV2::RegisterAppInterface_response* response = new NsAppLinkRPCV2::RegisterAppInterface_response();
                    const std::string& appName = object->get_appName();

                    if(AppMgrRegistry::getInstance().getItem(sessionKey))
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " Application " << appName << " is already registered!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_REGISTERED_ALREADY);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    Application_v2* app = (Application_v2*)core->getApplicationFromItemCheckNotNull(core->registerApplication( object, sessionKey ));
                    
                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    response->setMethodId(NsAppLinkRPCV2::FunctionID::RegisterAppInterfaceID);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " Application " << appName << " hasn't been registered!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPCV2::OnHMIStatus* status = new NsAppLinkRPCV2::OnHMIStatus();
                    status->set_hmiLevel(app->getApplicationHMIStatusLevel());
                    status->set_audioStreamingState(app->getApplicationAudioStreamingState());
                    status->set_systemContext(app->getSystemContext());
                    status->setMethodId(NsAppLinkRPCV2::FunctionID::OnHMIStatusID);
                    status->setMessageType(NsAppLinkRPC::ALRPCMessage::NOTIFICATION);
                    MobileHandler::getInstance().sendRPCMessage(status, sessionKey);

                    response->set_buttonCapabilities(core->mButtonCapabilitiesV2.get());
                    response->set_displayCapabilities(core->mDisplayCapabilitiesV2);
                    response->set_hmiZoneCapabilities(core->mHmiZoneCapabilitiesV2.get());
                    response->set_hmiDisplayLanguage(core->mUiLanguageV2);
                    response->set_language(core->mVrLanguageV2);
                    response->set_speechCapabilities(core->mSpeechCapabilitiesV2.get());
                    response->set_vrCapabilities(core->mVrCapabilitiesV2.get());
                    response->set_syncMsgVersion(app->getSyncMsgVersion());
                    response->set_softButtonCapabilities(core->mSoftButtonCapabilities.get());
                    response->set_presetBankCapabilities(core->mPresetBankCapabilities);
                    response->set_vehicleType(core->mVehicleType);
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);

                    LOG4CPLUS_INFO_EXT(mLogger, " A RegisterAppInterface response for the app "  << app->getName() << " gets sent to a mobile side... ");
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);

                    NsRPC2Communication::AppLinkCore::OnAppRegistered* appRegistered = new NsRPC2Communication::AppLinkCore::OnAppRegistered();
                    appRegistered->set_appName(app->getName());
                    appRegistered->set_isMediaApplication(app->getIsMediaApplication());
                    const NsAppLinkRPCV2::Language& languageDesired = app->getLanguageDesired();
                    NsAppLinkRPC::Language languageDesiredV1;
                    languageDesiredV1.set((NsAppLinkRPC::Language::LanguageInternal)languageDesired.get());
                    appRegistered->set_languageDesired(languageDesiredV1);
                    appRegistered->set_vrSynonym(app->getVrSynonyms());
                    appRegistered->set_appId(app->getAppID());
                    appRegistered->set_appType(app->getAppType());
                    const NsAppLinkRPCV2::Language& hmiLanguageDesired = app->getHMIDisplayLanguageDesired();
                    NsAppLinkRPC::Language hmiLanguageDesiredV1;
                    hmiLanguageDesiredV1.set((NsAppLinkRPC::Language::LanguageInternal)hmiLanguageDesired.get());
                    appRegistered->set_hmiDisplayLanguageDesired(hmiLanguageDesiredV1);
                    appRegistered->set_vrSynonym(app->getVrSynonyms());
                    appRegistered->set_deviceName(currentDeviceName);
                    appRegistered->set_versionNumber(2);
                    std::vector< NsAppLinkRPC::TTSChunk> ttsName;
                    for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = app->getTtsName().begin(); it != app->getTtsName().end(); it++)
                    {
                        const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                        NsAppLinkRPC::TTSChunk chunkV1;
                        chunkV1.set_text(chunk.get_text());
                        NsAppLinkRPC::SpeechCapabilities caps;
                        caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                        chunkV1.set_type(caps);
                        ttsName.push_back(chunkV1);
                    }
                    appRegistered->set_ttsName(ttsName);
                    HMIHandler::getInstance().sendNotification(appRegistered);
                    LOG4CPLUS_INFO_EXT(mLogger, " A RegisterAppInterface request was successful: registered an app " << app->getName());
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::UnregisterAppInterfaceID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An UnregisterAppInterface request has been invoked");
                    NsAppLinkRPCV2::UnregisterAppInterface_request * object = (NsAppLinkRPCV2::UnregisterAppInterface_request*)mobileMsg;
                    Application* app = core->getApplicationFromItemCheckNotNull(AppMgrRegistry::getInstance().getItem(sessionKey));
                    NsAppLinkRPCV2::UnregisterAppInterface_response* response = new NsAppLinkRPCV2::UnregisterAppInterface_response();
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    std::string appName = app->getName();

                    core->removeAppFromHmi(app, sessionKey);
                    core->unregisterApplication( sessionKey );

                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);

                    NsAppLinkRPCV2::OnAppInterfaceUnregistered* msgUnregistered = new NsAppLinkRPCV2::OnAppInterfaceUnregistered();
                    msgUnregistered->set_reason(NsAppLinkRPCV2::AppInterfaceUnregisteredReason(NsAppLinkRPCV2::AppInterfaceUnregisteredReason::USER_EXIT));
                    MobileHandler::getInstance().sendRPCMessage(msgUnregistered, sessionKey);
                    NsRPC2Communication::AppLinkCore::OnAppUnregistered* appUnregistered = new NsRPC2Communication::AppLinkCore::OnAppUnregistered();
                    appUnregistered->set_appName(appName);
                    appUnregistered->set_appId(app->getAppID());
                    appUnregistered->set_reason(NsAppLinkRPC::AppInterfaceUnregisteredReason((NsAppLinkRPC::AppInterfaceUnregisteredReason::AppInterfaceUnregisteredReasonInternal)NsAppLinkRPCV2::AppInterfaceUnregisteredReason::USER_EXIT));
                    HMIHandler::getInstance().sendNotification(appUnregistered);
                    LOG4CPLUS_INFO_EXT(mLogger, " An application " << appName << " has been unregistered successfully ");
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SubscribeButtonID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SubscribeButton request has been invoked");
                    NsAppLinkRPCV2::SubscribeButton_request * object = (NsAppLinkRPCV2::SubscribeButton_request*)mobileMsg;
                    NsAppLinkRPCV2::SubscribeButton_response* response = new NsAppLinkRPCV2::SubscribeButton_response();
                    RegistryItem* item = AppMgrRegistry::getInstance().getItem(sessionKey);
                    if(!item)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    core->mButtonsMapping.addButton( object->get_buttonName(), item );                    
                    response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                    response->setMethodId(NsAppLinkRPCV2::FunctionID::SubscribeButtonID);
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::UnsubscribeButtonID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An UnsubscribeButton request has been invoked");
                    NsAppLinkRPCV2::UnsubscribeButton_request * object = (NsAppLinkRPCV2::UnsubscribeButton_request*)mobileMsg;
                    core->mButtonsMapping.removeButton( object->get_buttonName() );
                    NsAppLinkRPCV2::UnsubscribeButton_response* response = new NsAppLinkRPCV2::UnsubscribeButton_response();
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SetMediaClockTimerID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SetMediaClockTimer request has been invoked");
                    NsRPC2Communication::UI::SetMediaClockTimer* setTimer = new NsRPC2Communication::UI::SetMediaClockTimer();
                    setTimer->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    setTimer->set_appId(sessionKey);
                    core->mMessageMapping.addMessage(setTimer->getId(), sessionKey);

                    NsAppLinkRPCV2::SetMediaClockTimer_request* object = (NsAppLinkRPCV2::SetMediaClockTimer_request*)mobileMsg;
                    if(object->get_startTime())
                    {
                        NsAppLinkRPC::StartTime startTime;
                        const NsAppLinkRPCV2::StartTime& startTimeV2 = *object->get_startTime();
                        startTime.set_hours(startTimeV2.get_hours());
                        startTime.set_minutes(startTimeV2.get_minutes());
                        startTime.set_seconds(startTimeV2.get_seconds());
                        setTimer->set_startTime(startTime);
                    }

                    const NsAppLinkRPCV2::UpdateMode& updateModeV2 = object->get_updateMode();
                    NsAppLinkRPC::UpdateMode updateMode;
                    updateMode.set((NsAppLinkRPC::UpdateMode::UpdateModeInternal)updateModeV2.get());
                    setTimer->set_updateMode(updateMode);
                    HMIHandler::getInstance().sendRequest(setTimer);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::PutFileID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An PutFile request has been invoked");
                    NsAppLinkRPCV2::PutFile_request* object = (NsAppLinkRPCV2::PutFile_request*)mobileMsg;
                    NsAppLinkRPCV2::PutFile_response* response = new NsAppLinkRPCV2::PutFile_response;

                    unsigned long int freeSpace = getAvailableSpace();
                    const std::string& syncFileName = object->get_syncFileName();
                    const NsAppLinkRPCV2::FileType& fileType = object->get_fileType();
                    bool persistentFile = object->get_persistentFile();
                    const std::vector<unsigned char>* fileData = object->getBinaryData();

                    bool isSyncFileName = !syncFileName.empty();
                    bool isFileData = fileData && !fileData->empty();
                    if (isSyncFileName && isFileData)
                    {
                        bool flag = false;
                        if (freeSpace > fileData->size())
                        {
                            struct stat status;
                            memset(&status, 0, sizeof(status));
                            if (stat(syncFileName.c_str(), &status) == -1) // File doesn't exist
                            {
                                Application* app = core->getApplicationFromItemCheckNotNull(
                                                        AppMgrRegistry::getInstance().getItem(sessionKey));

                                const std::string& name = app->getName();
                                const int& id = app->getAppID();

                                char path[FILENAME_MAX];
                                memset(path, 0, FILENAME_MAX);
                                snprintf(path, FILENAME_MAX - 1, "%s_%d/%s", name.c_str(), id, syncFileName.c_str());
                                std::ofstream file(path, std::ios_base::binary);
                                if (file.is_open())
                                {
                                    for (int i = 0; i < fileData->size(); ++i)
                                    {
                                        file << fileData->operator[](i);
                                    }
                                    file.close();
                                    flag = true;
                                }
                            }
                        }

                        if (flag)
                        {
                            response->set_success(true);
                            response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                            response->set_spaceAvailable(freeSpace);
                        }
                        else
                        {
                            response->set_success(false);
                            response->set_resultCode(NsAppLinkRPCV2::Result::GENERIC_ERROR);
                            response->set_spaceAvailable(freeSpace);
                        }
                    }
                    else
                    {
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                        response->set_spaceAvailable(freeSpace);
                    }

                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::DeleteFileID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An DeleteFile request has been invoked");
                    NsAppLinkRPCV2::DeleteFile_request* object = (NsAppLinkRPCV2::DeleteFile_request*)mobileMsg;
                    NsAppLinkRPCV2::DeleteFile_response* response = new NsAppLinkRPCV2::DeleteFile_response;

                    unsigned long int freeSpace = getAvailableSpace();
                    const std::string& syncFileName = object->get_syncFileName();
                    if(!syncFileName.empty())
                    {
                        Application* app = core->getApplicationFromItemCheckNotNull(
                                    AppMgrRegistry::getInstance().getItem(sessionKey));

                        const std::string& name = app->getName();
                        const int& id = app->getAppID();

                        char path[FILENAME_MAX];
                        memset(path, 0, FILENAME_MAX);
                        snprintf(path, FILENAME_MAX - 1, "%s_%d/%s", name.c_str(), id, syncFileName.c_str());
                        if(remove(path) != 0)
                        {
                            response->set_success(false);
                            response->set_resultCode(NsAppLinkRPCV2::Result::GENERIC_ERROR);
                            response->set_spaceAvailable(freeSpace);
                        }
                        else
                        {
                            response->set_success(true);
                            response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                            response->set_spaceAvailable(freeSpace);
                        }
                    }
                    else
                    {
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                        response->set_spaceAvailable(freeSpace);
                    }

                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::ListFilesID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An ListFiles request has been invoked");
                    NsAppLinkRPCV2::ListFiles_request* object = (NsAppLinkRPCV2::ListFiles_request*)mobileMsg;
                    NsAppLinkRPCV2::ListFiles_response* response = new NsAppLinkRPCV2::ListFiles_response;

                    std::vector<std::string> listFiles;
                    unsigned long int freeSpace = getAvailableSpace();

                    Application* app = core->getApplicationFromItemCheckNotNull(
                                    AppMgrRegistry::getInstance().getItem(sessionKey));

                    const std::string& name = app->getName();
                    const int& id = app->getAppID();

                    char path[FILENAME_MAX];
                    memset(path, 0, FILENAME_MAX);
                    snprintf(path, FILENAME_MAX - 1, "%s_%d/", name.c_str(), id);

                    DIR* dir = NULL;
                    struct dirent* dirElement = NULL;
                    memset(dirElement, 0, sizeof(dirent));
                    dir = opendir(path);
                    if (dir != NULL)
                    {
                        while (dirElement = readdir(dir))
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, " file: " << dirElement->d_name);
                            listFiles.push_back(std::string(dirElement->d_name));
                        }
                        closedir(dir);

                        response->set_filenames(listFiles);
                        response->set_success(true);
                        response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                        response->set_spaceAvailable(freeSpace);
                    }
                    else
                    {
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::GENERIC_ERROR);
                        response->set_spaceAvailable(freeSpace);
                    }

                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SliderID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A Slider request has been invoked");
                    NsAppLinkRPCV2::Slider_request* request = (NsAppLinkRPCV2::Slider_request*)mobileMsg;
                    NsRPC2Communication::UI::Slider* slider = new NsRPC2Communication::UI::Slider();

                    slider->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(slider->getId(), sessionKey);

                    slider->set_numTicks(request->get_numTicks());

                    slider->set_sliderHeader(request->get_sliderHeader());

                    if (request->get_sliderFooter())
                    {
                        slider->set_sliderFooter(*(request->get_sliderFooter()));
                    }

                    slider->set_position(request->get_position());
                    slider->set_timeout(request->get_timeout());
                    slider->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(slider);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SetAppIconID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SetAppIcon request has been invoked");
                    NsAppLinkRPCV2::SetAppIcon_request* request = static_cast<NsAppLinkRPCV2::SetAppIcon_request*>(mobileMsg);
                    Application* app = core->getApplicationFromItemCheckNotNull(
                        AppMgrRegistry::getInstance().getItem(sessionKey));
                    if (!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                        return;
                    }

                    NsRPC2Communication::UI::SetAppIcon* setAppIcon = new NsRPC2Communication::UI::SetAppIcon();
                    setAppIcon->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    setAppIcon->set_syncFileName(request->get_syncFileName());
                    setAppIcon->set_appId(app->getAppID());

                    core->mMessageMapping.addMessage(setAppIcon->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(setAppIcon);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::ScrollableMessageID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A ScrollableMessageID request has been invoked");
                    NsAppLinkRPCV2::ScrollableMessage_request* request = static_cast<NsAppLinkRPCV2::ScrollableMessage_request*>(mobileMsg);
                    Application* app = core->getApplicationFromItemCheckNotNull(
                        AppMgrRegistry::getInstance().getItem(sessionKey));
                    if (!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                        return;
                    }

                    NsRPC2Communication::UI::ScrollableMessage* scrollableMessage = new NsRPC2Communication::UI::ScrollableMessage();
                    if (!scrollableMessage)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, "Can't create scrollable message object.");
                        return;
                    }
                    scrollableMessage->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    scrollableMessage->set_appId(app->getAppID());
                    scrollableMessage->set_scrollableMessageBody(request->get_scrollableMessageBody());
                    if (request->get_timeout())
                    {
                        scrollableMessage->set_timeout(*(request->get_timeout()));
                    }
                    if ( request->get_softButtons() )
                    {
                        scrollableMessage->set_softButtons(*(request->get_softButtons()));
                    }
                    core->mMessageMapping.addMessage(scrollableMessage->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(scrollableMessage);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::EncodedSyncPDataID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An EncodedSyncPData request has been invoked");
                    NsAppLinkRPCV2::EncodedSyncPData_request* object = (NsAppLinkRPCV2::EncodedSyncPData_request*)mobileMsg;
                    NsAppLinkRPCV2::EncodedSyncPData_response* response = new NsAppLinkRPCV2::EncodedSyncPData_response;
                    Application* app = core->getApplicationFromItemCheckNotNull( AppMgrRegistry::getInstance().getItem(sessionKey) );
                    const std::string& name = app->getName();
                    core->mSyncPManager.setPData(object->get_data(), name, object->getMethodId());
                    response->set_success(true);
                    response->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);

                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SetGlobalPropertiesID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A SetGlobalProperties request has been invoked");
                    NsAppLinkRPCV2::SetGlobalProperties_request* object = (NsAppLinkRPCV2::SetGlobalProperties_request*)mobileMsg;
                    NsRPC2Communication::UI::SetGlobalProperties* setGPRPC2Request = new NsRPC2Communication::UI::SetGlobalProperties();
                    setGPRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(setGPRPC2Request->getId(), sessionKey);
                    if(object->get_helpPrompt())
                    {
                        std::vector< NsAppLinkRPC::TTSChunk> helpPrompt;
                        for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_helpPrompt()->begin(); it != object->get_helpPrompt()->end(); it++)
                        {
                            const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                            NsAppLinkRPC::TTSChunk chunkV1;
                            chunkV1.set_text(chunk.get_text());
                            NsAppLinkRPC::SpeechCapabilities caps;
                            caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                            chunkV1.set_type(caps);
                            helpPrompt.push_back(chunkV1);
                        }
                        setGPRPC2Request->set_helpPrompt(helpPrompt);
                    }

                    if(object->get_timeoutPrompt())
                    {
                        std::vector< NsAppLinkRPC::TTSChunk> timeoutPrompt;
                        for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_timeoutPrompt()->begin(); it != object->get_timeoutPrompt()->end(); it++)
                        {
                            const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                            NsAppLinkRPC::TTSChunk chunkV1;
                            chunkV1.set_text(chunk.get_text());
                            NsAppLinkRPC::SpeechCapabilities caps;
                            caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                            chunkV1.set_type(caps);
                            timeoutPrompt.push_back(chunkV1);
                        }
                        setGPRPC2Request->set_timeoutPrompt(timeoutPrompt);
                    }

                    if(object->get_vrHelp())
                    {
                        setGPRPC2Request->set_vrHelp(*object->get_vrHelp());
                    }
                    if(object->get_vrHelpTitle())
                    {
                        setGPRPC2Request->set_vrHelpTitle(*object->get_vrHelpTitle());
                    }

                    setGPRPC2Request->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(setGPRPC2Request);
                    NsAppLinkRPCV2::SetGlobalProperties_response * mobileResponse = new NsAppLinkRPCV2::SetGlobalProperties_response;
                    mobileResponse->set_success(true);
                    mobileResponse->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::ResetGlobalPropertiesID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A ResetGlobalProperties request has been invoked");
                    NsAppLinkRPCV2::ResetGlobalProperties_request* object = (NsAppLinkRPCV2::ResetGlobalProperties_request*)mobileMsg;
                    NsRPC2Communication::UI::ResetGlobalProperties* resetGPRPC2Request = new NsRPC2Communication::UI::ResetGlobalProperties();
                    resetGPRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(resetGPRPC2Request->getId(), sessionKey);
                    std::vector< NsAppLinkRPC::GlobalProperty> gp;
                    for(std::vector< NsAppLinkRPCV2::GlobalProperty>::const_iterator it = object->get_properties().begin(); it != object->get_properties().end(); it++)
                    {
                        const NsAppLinkRPCV2::GlobalProperty& prop = *it;
                        NsAppLinkRPC::GlobalProperty propV1;
                        propV1.set((NsAppLinkRPC::GlobalProperty::GlobalPropertyInternal)prop.get());
                        gp.push_back(propV1);
                    }
                    resetGPRPC2Request->set_properties(gp);
                    resetGPRPC2Request->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(resetGPRPC2Request);
                    NsAppLinkRPCV2::ResetGlobalProperties_response * mobileResponse = new NsAppLinkRPCV2::ResetGlobalProperties_response;
                    mobileResponse->set_success(true);
                    mobileResponse->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::CreateInteractionChoiceSetID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A CreateInteractionChoiceSet request has been invoked");
                    NsAppLinkRPCV2::CreateInteractionChoiceSet_request* object = (NsAppLinkRPCV2::CreateInteractionChoiceSet_request*)mobileMsg;
                    Application_v2* app = (Application_v2*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::CreateInteractionChoiceSet_response* response = new NsAppLinkRPCV2::CreateInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::CreateInteractionChoiceSet* createInteractionChoiceSet = new NsRPC2Communication::UI::CreateInteractionChoiceSet();
                    createInteractionChoiceSet->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(createInteractionChoiceSet->getId(), sessionKey);

                    const std::vector<NsAppLinkRPCV2::Choice>& choicesV2 = object->get_choiceSet();
                    std::vector<NsAppLinkRPC::Choice> choices;
                    for(std::vector<NsAppLinkRPCV2::Choice>::const_iterator it = choicesV2.begin(); it != choicesV2.end(); it++)
                    {
                        const NsAppLinkRPCV2::Choice& choiceV2 = *it;
                        NsAppLinkRPC::Choice choice;
                        choice.set_choiceID(choiceV2.get_choiceID());
                        choice.set_menuName(choiceV2.get_menuName());
                        choice.set_vrCommands(choiceV2.get_vrCommands());
                        choices.push_back(choice);
                    }
                    createInteractionChoiceSet->set_choiceSet(choices);
                    createInteractionChoiceSet->set_interactionChoiceSetID(object->get_interactionChoiceSetID());
                    createInteractionChoiceSet->set_appId(app->getAppID());
                    app->addChoiceSet(object->get_interactionChoiceSetID(), object->get_choiceSet());
                    HMIHandler::getInstance().sendRequest(createInteractionChoiceSet);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::DeleteInteractionChoiceSetID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteInteractionChoiceSet request has been invoked");
                    NsAppLinkRPCV2::DeleteInteractionChoiceSet_request* object = (NsAppLinkRPCV2::DeleteInteractionChoiceSet_request*)mobileMsg;
                    Application_v2* app = (Application_v2*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPCV2::DeleteInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& choiceSetId = object->get_interactionChoiceSetID();
                    const ChoiceSetV2* choiceSetFound = app->findChoiceSet(choiceSetId);
                    if(!choiceSetFound)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " a choice set " << choiceSetId
                                            << " hasn't been registered within the application " << app->getName() << " id" << app->getAppID() << " !");
                        NsAppLinkRPCV2::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPCV2::DeleteInteractionChoiceSet_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        return;
                    }

                    NsRPC2Communication::UI::DeleteInteractionChoiceSet* deleteInteractionChoiceSet = new NsRPC2Communication::UI::DeleteInteractionChoiceSet();
                    deleteInteractionChoiceSet->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(deleteInteractionChoiceSet->getId(), sessionKey);
                    deleteInteractionChoiceSet->set_interactionChoiceSetID(object->get_interactionChoiceSetID());
                    deleteInteractionChoiceSet->set_appId(app->getAppID());
                    app->removeChoiceSet(object->get_interactionChoiceSetID());
                    HMIHandler::getInstance().sendRequest(deleteInteractionChoiceSet);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::PerformInteractionID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A PerformInteraction request has been invoked");
                    NsAppLinkRPCV2::PerformInteraction_request* object = (NsAppLinkRPCV2::PerformInteraction_request*)mobileMsg;
                    Application_v2* app = (Application_v2*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::PerformInteraction_response* response = new NsAppLinkRPCV2::PerformInteraction_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const std::vector<unsigned int>& choiceSets = object->get_interactionChoiceSetIDList();
                    for(std::vector<unsigned int>::const_iterator it = choiceSets.begin(); it != choiceSets.end(); it++)
                    {
                        const unsigned int& choiceSetId = *it;
                        const ChoiceSetV2* choiceSetFound = app->findChoiceSet(choiceSetId);
                        if(!choiceSetFound)
                        {
                            LOG4CPLUS_ERROR_EXT(mLogger, " a choice set " << choiceSetId
                                                << " hasn't been registered within the application " << app->getName() << " id" << app->getAppID() << " !");
                            NsAppLinkRPCV2::PerformInteraction_response* response = new NsAppLinkRPCV2::PerformInteraction_response;
                            response->set_success(false);
                            response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                            MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                            return;
                        }
                    }
                    NsRPC2Communication::UI::PerformInteraction* performInteraction = new NsRPC2Communication::UI::PerformInteraction();
                    performInteraction->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(performInteraction->getId(), sessionKey);
                    if(object->get_helpPrompt())
                    {
                        std::vector< NsAppLinkRPC::TTSChunk> helpPrompt;
                        for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_helpPrompt()->begin(); it != object->get_helpPrompt()->end(); it++)
                        {
                            const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                            NsAppLinkRPC::TTSChunk chunkV1;
                            chunkV1.set_text(chunk.get_text());
                            NsAppLinkRPC::SpeechCapabilities caps;
                            caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                            chunkV1.set_type(caps);
                            helpPrompt.push_back(chunkV1);
                        }
                        performInteraction->set_helpPrompt(helpPrompt);
                    }
                    std::vector< NsAppLinkRPC::TTSChunk> initialPrompt;
                    for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_initialPrompt().begin(); it != object->get_initialPrompt().end(); it++)
                    {
                        const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                        NsAppLinkRPC::TTSChunk chunkV1;
                        chunkV1.set_text(chunk.get_text());
                        NsAppLinkRPC::SpeechCapabilities caps;
                        caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                        chunkV1.set_type(caps);
                        initialPrompt.push_back(chunkV1);
                    }
                    performInteraction->set_initialPrompt(initialPrompt);
                    performInteraction->set_initialText(object->get_initialText());
                    performInteraction->set_interactionChoiceSetIDList(choiceSets);
                    const NsAppLinkRPCV2::InteractionMode& interactionMode = object->get_interactionMode();
                    NsAppLinkRPC::InteractionMode interactionModeV1;
                    interactionModeV1.set((NsAppLinkRPC::InteractionMode::InteractionModeInternal)interactionMode.get());
                    performInteraction->set_interactionMode(interactionModeV1);
                    if(object->get_timeout())
                    {
                        performInteraction->set_timeout(*object->get_timeout());
                    }
                    if(object->get_timeoutPrompt())
                    {
                        std::vector< NsAppLinkRPC::TTSChunk> timeoutPrompt;
                        for(std::vector< NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_timeoutPrompt()->begin(); it != object->get_timeoutPrompt()->end(); it++)
                        {
                            const NsAppLinkRPCV2::TTSChunk& chunk = *it;
                            NsAppLinkRPC::TTSChunk chunkV1;
                            chunkV1.set_text(chunk.get_text());
                            NsAppLinkRPC::SpeechCapabilities caps;
                            caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)chunk.get_type().get());
                            chunkV1.set_type(caps);
                            timeoutPrompt.push_back(chunkV1);
                        }
                        performInteraction->set_timeoutPrompt(timeoutPrompt);
                    }
                    performInteraction->set_appId(sessionKey);
                    if(object->get_vrHelp())
                    {
                        performInteraction->set_vrHelp(*object->get_vrHelp());
                    }
                    HMIHandler::getInstance().sendRequest(performInteraction);
                }
                case NsAppLinkRPCV2::FunctionID::AlertID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An Alert request has been invoked");
                    NsAppLinkRPCV2::Alert_request* object = (NsAppLinkRPCV2::Alert_request*)mobileMsg;
                    NsRPC2Communication::UI::Alert* alert = new NsRPC2Communication::UI::Alert();
                    alert->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(alert->getId(), sessionKey);
                    if(object->get_alertText1())
                    {
                        alert->set_AlertText1(*object->get_alertText1());
                    }
                    if(object->get_alertText2())
                    {
                        alert->set_AlertText2(*object->get_alertText2());
                    }
                    if(object->get_alertText3())
                    {
                        alert->set_alertText3(*object->get_alertText3());
                    }
                    if(object->get_duration())
                    {
                        alert->set_duration(*object->get_duration());
                    }
                    if(object->get_playTone())
                    {
                        alert->set_playTone(*object->get_playTone());
                    }
                    if(object->get_softButtons())
                    {
                        alert->set_softButtons(*object->get_softButtons());
                    }
                    alert->set_appId(sessionKey);
                    HMIHandler::getInstance().sendRequest(alert);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::ShowID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A Show request has been invoked");
                    LOG4CPLUS_INFO_EXT(mLogger, "message " << mobileMsg->getMethodId() );
                    NsAppLinkRPCV2::Show_request* object = (NsAppLinkRPCV2::Show_request*)mobileMsg;
                    NsRPC2Communication::UI::Show* showRPC2Request = new NsRPC2Communication::UI::Show();
                    showRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    LOG4CPLUS_INFO_EXT(mLogger, "showrpc2request created");
                    if(object->get_mainField1())
                    {
                        showRPC2Request->set_mainField1(*object->get_mainField1());
                    }
                    if(object->get_mainField2())
                    {
                        showRPC2Request->set_mainField2(*object->get_mainField2());
                    }
                    if(object->get_mainField3())
                    {
                        showRPC2Request->set_mainField1(*object->get_mainField3());
                    }
                    if(object->get_mainField4())
                    {
                        showRPC2Request->set_mainField1(*object->get_mainField4());
                    }
                    if(object->get_mediaClock())
                    {
                        showRPC2Request->set_mediaClock(*object->get_mediaClock());
                    }
                    if(object->get_mediaTrack())
                    {
                        showRPC2Request->set_mediaTrack(*object->get_mediaTrack());
                    }
                    if(object->get_statusBar())
                    {
                        showRPC2Request->set_statusBar(*object->get_statusBar());
                    }
                    if(object->get_graphic())
                    {
                        showRPC2Request->set_graphic(*object->get_graphic());
                    }
                    if(object->get_softButtons())
                    {
                        showRPC2Request->set_softButtons(*object->get_softButtons());
                    }
                    if(object->get_customPresets())
                    {
                        showRPC2Request->set_customPresets(*object->get_customPresets());
                    }
                    if(object->get_alignment())
                    {
                        const NsAppLinkRPCV2::TextAlignment& textAlignment = *object->get_alignment();
                        NsAppLinkRPC::TextAlignment textAlignmentV1;
                        textAlignmentV1.set((NsAppLinkRPC::TextAlignment::TextAlignmentInternal)textAlignment.get());
                        showRPC2Request->set_alignment(textAlignmentV1);
                    }
                    showRPC2Request->set_appId(sessionKey);
                    LOG4CPLUS_INFO_EXT(mLogger, "Show request almost handled" );
                    core->mMessageMapping.addMessage(showRPC2Request->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(showRPC2Request);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::SpeakID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A Speak request has been invoked");
                    NsAppLinkRPCV2::Speak_request* object = (NsAppLinkRPCV2::Speak_request*)mobileMsg;
                    NsRPC2Communication::TTS::Speak* speakRPC2Request = new NsRPC2Communication::TTS::Speak();
                    speakRPC2Request->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    std::vector<NsAppLinkRPC::TTSChunk> ttsChunks;
                    for(std::vector<NsAppLinkRPCV2::TTSChunk>::const_iterator it = object->get_ttsChunks().begin(); it != object->get_ttsChunks().end(); it++)
                    {
                        const NsAppLinkRPCV2::TTSChunk& ttsChunkV2 = *it;
                        NsAppLinkRPC::TTSChunk ttsChunkV1;
                        ttsChunkV1.set_text(ttsChunkV2.get_text());
                        NsAppLinkRPC::SpeechCapabilities caps;
                        caps.set((NsAppLinkRPC::SpeechCapabilities::SpeechCapabilitiesInternal)ttsChunkV2.get_type().get());
                        ttsChunkV1.set_type(caps);
                        ttsChunks.push_back(ttsChunkV1);
                    }
                    speakRPC2Request->set_ttsChunks(ttsChunks);
                    speakRPC2Request->set_appId(sessionKey);
                    core->mMessageMapping.addMessage(speakRPC2Request->getId(), sessionKey);
                    HMIHandler::getInstance().sendRequest(speakRPC2Request);
                    /*NsAppLinkRPCV2::Speak_response * mobileResponse = new NsAppLinkRPCV2::Speak_response;
                    mobileResponse->set_resultCode(NsAppLinkRPCV2::Result::SUCCESS);
                    mobileResponse->set_success(true);
                    MobileHandler::getInstance().sendRPCMessage(mobileResponse, sessionKey);*/
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::AddCommandID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand request has been invoked");

                    Application_v2* app = (Application_v2*)AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::AddCommand_response* response = new NsAppLinkRPCV2::AddCommand_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsAppLinkRPCV2::AddCommand_request* object = (NsAppLinkRPCV2::AddCommand_request*)mobileMsg;

                    const unsigned int& cmdId = object->get_cmdID();

                    if(object->get_menuParams())
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand UI request has been invoked");
                        NsRPC2Communication::UI::AddCommand * addCmd = new NsRPC2Communication::UI::AddCommand();
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        CommandType cmdType = CommandType::UI;
                        const NsAppLinkRPCV2::MenuParams* menuParams = object->get_menuParams();
                        NsAppLinkRPC::MenuParams menuParamsV1;
                        menuParamsV1.set_menuName(menuParams->get_menuName());
                        if(menuParams->get_parentID())
                        {
                            menuParamsV1.set_parentID(*menuParams->get_parentID());
                        }
                        if(menuParams->get_position())
                        {
                            menuParamsV1.set_position(*menuParams->get_position());
                        }
                        addCmd->set_menuParams(menuParamsV1);
                        addCmd->set_cmdId(cmdId);
                        addCmd->set_appId(app->getAppID());
                        if(menuParams->get_parentID())
                        {
                            const unsigned int& menuId = *menuParams->get_parentID();
                            app->addMenuCommand(cmdId, menuId);
                        }
                        core->mMessageMapping.addMessage(addCmd->getId(), sessionKey);

                        if(object->get_cmdIcon())
                        {
                            addCmd->set_cmdIcon(*object->get_cmdIcon());
                        }

                        CommandParams params;
                        params.menuParamsV2 = menuParams;
                        app->addCommand(cmdId, cmdType, params);
                        app->incrementUnrespondedRequestCount(cmdId);
                        core->mRequestMapping.addMessage(addCmd->getId(), cmdId);
                        HMIHandler::getInstance().sendRequest(addCmd);

                    }
                    if(object->get_vrCommands())
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand VR request has been invoked");
                        NsRPC2Communication::VR::AddCommand * addCmd = new NsRPC2Communication::VR::AddCommand();
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        CommandType cmdType = CommandType::VR;
                        addCmd->set_vrCommands(*object->get_vrCommands());
                        addCmd->set_cmdId(cmdId);
                        addCmd->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(addCmd->getId(), sessionKey);
                        CommandParams params;
                        params.vrCommands = object->get_vrCommands();
                        app->addCommand(cmdId, cmdType, params);
                        app->incrementUnrespondedRequestCount(cmdId);
                        core->mRequestMapping.addMessage(addCmd->getId(), cmdId);
                        HMIHandler::getInstance().sendRequest(addCmd);
                    }

                    break;
                }
                case NsAppLinkRPCV2::FunctionID::DeleteCommandID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand request has been invoked");
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::DeleteCommand_response* response = new NsAppLinkRPCV2::DeleteCommand_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPCV2::DeleteCommand_request* object = (NsAppLinkRPCV2::DeleteCommand_request*)mobileMsg;

                    CommandTypes cmdTypes = app->getCommandTypes(object->get_cmdID());
                    if(cmdTypes.empty())
                    {
                        NsAppLinkRPCV2::DeleteCommand_response* response = new NsAppLinkRPCV2::DeleteCommand_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& cmdId = object->get_cmdID();
                    for(CommandTypes::iterator it = cmdTypes.begin(); it != cmdTypes.end(); it++)
                    {
                        CommandType cmdType = *it;
                        if(cmdType == CommandType::UI)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand UI request has been invoked");
                            NsRPC2Communication::UI::DeleteCommand* deleteCmd = new NsRPC2Communication::UI::DeleteCommand();
                            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                            deleteCmd->set_appId(app->getAppID());
                            core->mMessageMapping.addMessage(deleteCmd->getId(), sessionKey);
                            deleteCmd->set_cmdId(cmdId);
                            app->removeCommand(cmdId, cmdType);
                            app->incrementUnrespondedRequestCount(cmdId);
                            app->removeMenuCommand(cmdId);
                            core->mRequestMapping.addMessage(deleteCmd->getId(), cmdId);
                            HMIHandler::getInstance().sendRequest(deleteCmd);
                        }
                        else if(cmdType == CommandType::VR)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand VR request has been invoked");
                            NsRPC2Communication::VR::DeleteCommand* deleteCmd = new NsRPC2Communication::VR::DeleteCommand();
                            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                            core->mMessageMapping.addMessage(deleteCmd->getId(), sessionKey);
                            deleteCmd->set_cmdId(cmdId);
                            deleteCmd->set_appId(app->getAppID());
                            app->removeCommand(cmdId, cmdType);
                            app->incrementUnrespondedRequestCount(cmdId);
                            core->mRequestMapping.addMessage(deleteCmd->getId(), cmdId);
                            HMIHandler::getInstance().sendRequest(deleteCmd);
                        }
                    }
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::AddSubMenuID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " An AddSubmenu request has been invoked");
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::AddSubMenu_response* response = new NsAppLinkRPCV2::AddSubMenu_response();
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }

                    NsAppLinkRPCV2::AddSubMenu_request* object = (NsAppLinkRPCV2::AddSubMenu_request*)mobileMsg;
                    NsRPC2Communication::UI::AddSubMenu* addSubMenu = new NsRPC2Communication::UI::AddSubMenu();
                    addSubMenu->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(addSubMenu->getId(), sessionKey);
                    addSubMenu->set_menuId(object->get_menuID());
                    addSubMenu->set_menuName(object->get_menuName());
                    if(object->get_position())
                    {
                        addSubMenu->set_position(*object->get_position());
                    }
                    addSubMenu->set_appId(app->getAppID());
                    app->addMenu(object->get_menuID(), object->get_menuName(), object->get_position());
                    HMIHandler::getInstance().sendRequest(addSubMenu);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::DeleteSubMenuID:
                {
                    LOG4CPLUS_INFO_EXT(mLogger, " A DeleteSubmenu request has been invoked");
                    NsAppLinkRPCV2::DeleteSubMenu_request* object = (NsAppLinkRPCV2::DeleteSubMenu_request*)mobileMsg;
                    Application* app = AppMgrRegistry::getInstance().getApplication(sessionKey);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " session key " << sessionKey
                            << " hasn't been associated with any application!");
                        NsAppLinkRPCV2::DeleteSubMenu_response* response = new NsAppLinkRPCV2::DeleteSubMenu_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    const unsigned int& menuId = object->get_menuID();
                    if(!app->findMenu(menuId))
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " menuId " << menuId
                                            << " hasn't been associated with the application " << app->getName() << " id " << app->getAppID() << " !");
                        NsAppLinkRPCV2::DeleteSubMenu_response* response = new NsAppLinkRPCV2::DeleteSubMenu_response;
                        response->set_success(false);
                        response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                        MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                        break;
                    }
                    NsRPC2Communication::UI::DeleteSubMenu* delSubMenu = new NsRPC2Communication::UI::DeleteSubMenu();
                    delSubMenu->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    core->mMessageMapping.addMessage(delSubMenu->getId(), sessionKey);
                    delSubMenu->set_menuId(menuId);
                    delSubMenu->set_appId(app->getAppID());
                    const MenuCommands& menuCommands = app->findMenuCommands(menuId);
                    LOG4CPLUS_INFO_EXT(mLogger, " A given menu has " << menuCommands.size() << " UI commands - about to delete 'em!");
                    for(MenuCommands::const_iterator it = menuCommands.begin(); it != menuCommands.end(); it++)
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, " Deleting command with id " << *it);
                        NsRPC2Communication::UI::DeleteCommand* delUiCmd = new NsRPC2Communication::UI::DeleteCommand();
                        delUiCmd->set_cmdId(*it);
                        delUiCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        delUiCmd->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(delUiCmd->getId(), sessionKey);
                        core->mRequestMapping.addMessage(delUiCmd->getId(), *it);
                        HMIHandler::getInstance().sendRequest(delUiCmd);
                        const CommandTypes& types = app->getCommandTypes(*it);
                        for(CommandTypes::const_iterator it2 = types.begin(); it2 != types.end(); it2++)
                        {
                            const CommandType& type = *it2;
                            if(type == CommandType::VR)
                            {
                                LOG4CPLUS_INFO_EXT(mLogger, " A given command id " << *it << " has VR counterpart attached to: deleting it also!");
                                NsRPC2Communication::VR::DeleteCommand* delVrCmd = new NsRPC2Communication::VR::DeleteCommand();
                                delVrCmd->set_cmdId(*it);
                                delVrCmd->set_appId(app->getAppID());
                                core->mMessageMapping.addMessage(delVrCmd->getId(), sessionKey);
                                core->mRequestMapping.addMessage(delVrCmd->getId(), *it);
                                app->removeCommand(*it, CommandType::VR);
                                HMIHandler::getInstance().sendRequest(delVrCmd);
                            }
                        }
                        app->removeCommand(*it, CommandType::UI);
                        app->removeMenuCommand(*it);
                    }
                    app->removeMenu(menuId);
                    HMIHandler::getInstance().sendRequest(delSubMenu);
                    break;
                }
                case NsAppLinkRPCV2::FunctionID::INVALID_ENUM:
                default:
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, " An undefined or invalid RPC message " << mobileMsg->getMethodId() << " has been received!");
                    NsAppLinkRPCV2::GenericResponse_response* response = new NsAppLinkRPCV2::GenericResponse_response();
                    response->setMethodId(NsAppLinkRPCV2::FunctionID::GenericResponseID);
                    response->set_success(false);
                    response->set_resultCode(NsAppLinkRPCV2::Result::INVALID_DATA);
                    MobileHandler::getInstance().sendRPCMessage(response, sessionKey);
                    break;
                }
            }
        }
    }

    /**
     * \brief push HMI RPC2 message to a queue
     * \param msg a message to be pushed
     * \param pThis a pointer to AppMgrCore class instance
     */
    void AppMgrCore::handleBusRPCMessageIncoming(NsRPC2Communication::RPC2Command* msg , void *pThis)
    {
        if(!msg)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, " Incoming null pointer from HMI side!");
            return;
        }
        LOG4CPLUS_INFO_EXT(mLogger, " A RPC2 bus message " << msg->getMethod() << " has been incoming...");

        if(!pThis)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, " pThis should point to an instance of AppMgrCore class");
            return;
        }
        
        AppMgrCore* core = (AppMgrCore*)pThis;
        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_INVALID:
            {
                LOG4CPLUS_ERROR_EXT(mLogger, " An invalid RPC message " << msg->getMethod() << " has been received!");
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " A valid RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_BUTTONS__ONBUTTONEVENT:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnButtonEvent notification has been invoked");
                NsRPC2Communication::Buttons::OnButtonEvent * object = (NsRPC2Communication::Buttons::OnButtonEvent*)msg;

                NsAppLinkRPCV2::ButtonName btnName;
                btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)object->get_name().get());
                Application* app = core->getApplicationFromItemCheckNotNull(core->mButtonsMapping.findRegistryItemSubscribedToButton(btnName));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::OnButtonEvent* event = new NsAppLinkRPC::OnButtonEvent();
                        event->set_buttonEventMode(object->get_mode());
                        const NsAppLinkRPC::ButtonName & name = object->get_name();
                        event->set_buttonName(name);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName() << " Application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::OnButtonEvent* event = new NsAppLinkRPCV2::OnButtonEvent();
                        const NsAppLinkRPC::ButtonEventMode& evtMode = object->get_mode();
                        NsAppLinkRPCV2::ButtonEventMode evtModeV2;
                        evtModeV2.set((NsAppLinkRPCV2::ButtonEventMode::ButtonEventModeInternal)evtMode.get());
                        event->set_buttonEventMode(evtModeV2);
                        const NsAppLinkRPC::ButtonName& btnName = object->get_name();
                        NsAppLinkRPCV2::ButtonName btnNameV2;
                        btnNameV2.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)btnName.get());
                        event->set_buttonName(btnNameV2);
                        if(object->get_customButtonID())
                        {
                            event->set_customButtonID(*object->get_customButtonID());
                        }
                        else
                        {
                            event->set_customButtonID(0);
                        }
                        event->setMessageType(NsAppLinkRPC::ALRPCMessage::NOTIFICATION);
                        event->setMethodId(NsAppLinkRPCV2::FunctionID::OnButtonEventID);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName() << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_BUTTONS__ONBUTTONPRESS:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnButtonPress notification has been invoked");
                NsRPC2Communication::Buttons::OnButtonPress * object = (NsRPC2Communication::Buttons::OnButtonPress*)msg;
                const NsAppLinkRPC::ButtonName & name = object->get_name();
                NsAppLinkRPCV2::ButtonName btnName;
                btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)name.get());
                Application* app = core->getApplicationFromItemCheckNotNull(core->mButtonsMapping.findRegistryItemSubscribedToButton(btnName));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::OnButtonPress* event = new NsAppLinkRPC::OnButtonPress();

                        event->set_buttonName(name);
                        event->set_buttonPressMode(object->get_mode());
                        LOG4CPLUS_INFO_EXT(mLogger, "before we find sessionID");

                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::OnButtonPress* event = new NsAppLinkRPCV2::OnButtonPress();
                        NsAppLinkRPCV2::ButtonName btnName;
                        btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)name.get());
                        event->set_buttonName(btnName);
                        NsAppLinkRPCV2::ButtonPressMode pressMode;
                        pressMode.set((NsAppLinkRPCV2::ButtonPressMode::ButtonPressModeInternal)object->get_mode().get());
                        event->set_buttonPressMode(pressMode);
                        if(object->get_customButtonID())
                        {
                            event->set_customButtonID(*object->get_customButtonID());
                        }
                        else
                        {
                            event->set_customButtonID(0);
                        }
                        LOG4CPLUS_INFO_EXT(mLogger, "before we find sessionID");

                        event->setMessageType(NsAppLinkRPC::ALRPCMessage::NOTIFICATION);
                        event->setMethodId(NsAppLinkRPCV2::FunctionID::OnButtonPressID);

                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app "<< app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_BUTTONS__GETCAPABILITIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetButtonCapabilities response has been income");
                NsRPC2Communication::Buttons::GetCapabilitiesResponse * btnCaps = (NsRPC2Communication::Buttons::GetCapabilitiesResponse*)msg;
                core->mButtonCapabilitiesV1.set( btnCaps->get_capabilities() );
                std::vector< NsAppLinkRPCV2::ButtonCapabilities> caps;
                for(std::vector< NsAppLinkRPC::ButtonCapabilities>::const_iterator it = btnCaps->get_capabilities().begin(); it != btnCaps->get_capabilities().end(); it++)
                {
                    const NsAppLinkRPC::ButtonCapabilities& cap = *it;
                    NsAppLinkRPCV2::ButtonCapabilities capV2;
                    capV2.set_longPressAvailable(cap.get_longPressAvailable());
                    NsAppLinkRPCV2::ButtonName btnName;
                    btnName.set((NsAppLinkRPCV2::ButtonName::ButtonNameInternal)cap.get_name().get());
                    capV2.set_name(btnName);
                    capV2.set_shortPressAvailable(cap.get_shortPressAvailable());
                    capV2.set_upDownAvailable(cap.get_upDownAvailable());
                    caps.push_back(capV2);
                }
                core->mButtonCapabilitiesV2.set(caps);
                if(btnCaps->get_presetBankCapabilities())
                {
                    core->mPresetBankCapabilities = *btnCaps->get_presetBankCapabilities();
                }
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not Buttons RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ONREADY:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnReady UI notification has been invoked");
                HMIHandler::getInstance().setReadyState(true);

                NsRPC2Communication::UI::GetCapabilities* getUiCapsRequest = new NsRPC2Communication::UI::GetCapabilities();
                HMIHandler::getInstance().sendRequest(getUiCapsRequest);
                NsRPC2Communication::VR::GetCapabilities* getVrCapsRequest = new NsRPC2Communication::VR::GetCapabilities();
                HMIHandler::getInstance().sendRequest(getVrCapsRequest);
                NsRPC2Communication::TTS::GetCapabilities* getTtsCapsRequest = new NsRPC2Communication::TTS::GetCapabilities();
                HMIHandler::getInstance().sendRequest(getTtsCapsRequest);
                NsRPC2Communication::Buttons::GetCapabilities* getButtonsCapsRequest = new NsRPC2Communication::Buttons::GetCapabilities();
                HMIHandler::getInstance().sendRequest(getButtonsCapsRequest);
                NsRPC2Communication::VehicleInfo::GetVehicleType* getVehicleType = new NsRPC2Communication::VehicleInfo::GetVehicleType;
                HMIHandler::getInstance().sendRequest(getVehicleType);

                NsRPC2Communication::UI::GetLanguage* getUiLang = new NsRPC2Communication::UI::GetLanguage;
                HMIHandler::getInstance().sendRequest(getUiLang);
                NsRPC2Communication::VR::GetLanguage* getVrLang = new NsRPC2Communication::VR::GetLanguage;
                HMIHandler::getInstance().sendRequest(getVrLang);
                NsRPC2Communication::TTS::GetLanguage* getTtsLang = new NsRPC2Communication::TTS::GetLanguage;
                HMIHandler::getInstance().sendRequest(getTtsLang);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__GETCAPABILITIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetUICapabilities response has been income");
                NsRPC2Communication::UI::GetCapabilitiesResponse * uiCaps = (NsRPC2Communication::UI::GetCapabilitiesResponse*)msg;
                core->mDisplayCapabilitiesV1 = uiCaps->get_displayCapabilities();

                NsAppLinkRPCV2::DisplayCapabilities displayCaps;
                const NsAppLinkRPC::DisplayCapabilities& displayCapsV1 = uiCaps->get_displayCapabilities();
                NsAppLinkRPCV2::DisplayType displayType;
                displayType.set((NsAppLinkRPCV2::DisplayType::DisplayTypeInternal)displayCapsV1.get_displayType().get());
                displayCaps.set_displayType(displayType);
                std::vector<NsAppLinkRPCV2::MediaClockFormat> fmt;
                for(std::vector<NsAppLinkRPC::MediaClockFormat>::const_iterator it = displayCapsV1.get_mediaClockFormats().begin(); it != displayCapsV1.get_mediaClockFormats().end(); it++)
                {
                    NsAppLinkRPCV2::MediaClockFormat fmtItem;
                    fmtItem.set((NsAppLinkRPCV2::MediaClockFormat::MediaClockFormatInternal)(*it).get());
                    fmt.push_back(fmtItem);
                }
                displayCaps.set_mediaClockFormats(fmt);
                std::vector<NsAppLinkRPCV2::TextField> txtFields;
                for(std::vector<NsAppLinkRPC::TextField>::const_iterator it = displayCapsV1.get_textFields().begin(); it != displayCapsV1.get_textFields().end(); it++)
                {
                    NsAppLinkRPCV2::TextField txtField;
                    const NsAppLinkRPC::TextField txtFieldV1 = *it;
                    NsAppLinkRPCV2::CharacterSet charset;
                    charset.set((NsAppLinkRPCV2::CharacterSet::CharacterSetInternal)txtFieldV1.get_characterSet().get());
                    txtField.set_characterSet(charset);
                    NsAppLinkRPCV2::TextFieldName name;
                    name.set((NsAppLinkRPCV2::TextFieldName::TextFieldNameInternal)txtFieldV1.get_name().get());
                    txtField.set_name(name);
                    txtField.set_rows(txtFieldV1.get_rows());
                    txtField.set_width(txtFieldV1.get_width());
                    txtFields.push_back(txtField);
                }
                displayCaps.set_textFields(txtFields);
                core->mDisplayCapabilitiesV2 = displayCaps;
                core->mHmiZoneCapabilitiesV1.set( uiCaps->get_hmiZoneCapabilities() );

                std::vector< NsAppLinkRPCV2::HmiZoneCapabilities> hmiCaps;
                for(std::vector< NsAppLinkRPC::HmiZoneCapabilities>::const_iterator it = uiCaps->get_hmiZoneCapabilities().begin(); it != uiCaps->get_hmiZoneCapabilities().end(); it++)
                {
                    const NsAppLinkRPC::HmiZoneCapabilities& cap = *it;
                    NsAppLinkRPCV2::HmiZoneCapabilities capV2;
                    capV2.set((NsAppLinkRPCV2::HmiZoneCapabilities::HmiZoneCapabilitiesInternal)cap.get());
                    hmiCaps.push_back(capV2);
                }
                core->mHmiZoneCapabilitiesV2.set( hmiCaps );
                if(uiCaps->get_softButtonCapabilities())
                {
                    core->mSoftButtonCapabilities.set(*uiCaps->get_softButtonCapabilities());
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__GETLANGUAGERESPONSE:
            {
                NsRPC2Communication::UI::GetLanguageResponse* getLang = (NsRPC2Communication::UI::GetLanguageResponse*)msg;
                core->mUiLanguageV1 = getLang->get_hmiDisplayLanguage();
                NsAppLinkRPCV2::Language langV2;
                langV2.set((NsAppLinkRPCV2::Language::LanguageInternal)getLang->get_hmiDisplayLanguage().get());
                core->mUiLanguageV2 = langV2;
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ONCOMMAND:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnCommand UI notification has been invoked");
                NsRPC2Communication::UI::OnCommand* object = (NsRPC2Communication::UI::OnCommand*)msg;
                Application* app = AppMgrRegistry::getInstance().getApplicationByCommand(object->get_commandId());
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::OnCommand* event = new NsAppLinkRPC::OnCommand();
                        event->set_cmdID(object->get_commandId());
                        event->set_triggerSource(NsAppLinkRPC::TriggerSource::TS_MENU);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::OnCommand* event = new NsAppLinkRPCV2::OnCommand();
                        event->set_cmdID(object->get_commandId());
                        event->set_triggerSource(NsAppLinkRPCV2::TriggerSource::TS_MENU);
                        event->setMethodId(NsAppLinkRPCV2::FunctionID::OnCommandID);
                        event->setMessageType(NsAppLinkRPC::ALRPCMessage::NOTIFICATION);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SHOWRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A Show response has been income");
                NsRPC2Communication::UI::ShowResponse* object = (NsRPC2Communication::UI::ShowResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::Show_response* response = new NsAppLinkRPC::Show_response();
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::Show_response* response = new NsAppLinkRPCV2::Show_response();
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);
                        response->setMethodId(NsAppLinkRPCV2::FunctionID::ShowID);
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SETGLOBALPROPERTIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A SetGlobalProperties response has been income");
                NsRPC2Communication::UI::SetGlobalPropertiesResponse* object = (NsRPC2Communication::UI::SetGlobalPropertiesResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::SetGlobalProperties_response* response = new NsAppLinkRPC::SetGlobalProperties_response();

                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::SetGlobalProperties_response* response = new NsAppLinkRPCV2::SetGlobalProperties_response();

                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__RESETGLOBALPROPERTIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A ResetGlobalProperties response has been income");
                NsRPC2Communication::UI::ResetGlobalPropertiesResponse* object = (NsRPC2Communication::UI::ResetGlobalPropertiesResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::ResetGlobalProperties_response* response = new NsAppLinkRPC::ResetGlobalProperties_response();

                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::ResetGlobalProperties_response* response = new NsAppLinkRPCV2::ResetGlobalProperties_response();

                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ALERTRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An Alert response has been income");
                NsRPC2Communication::UI::AlertResponse* object = (NsRPC2Communication::UI::AlertResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::Alert_response* response = new NsAppLinkRPC::Alert_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::Alert_response* response = new NsAppLinkRPCV2::Alert_response();
                        response->set_success(true);                        
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->setMethodId(NsAppLinkRPCV2::FunctionID::AlertID);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        if(object->get_tryAgainTime())
                        {
                            response->set_tryAgainTime(*object->get_tryAgainTime());
                        }

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ADDCOMMANDRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand UI response has been income");
                NsRPC2Communication::UI::AddCommandResponse* object = (NsRPC2Communication::UI::AddCommandResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                unsigned int cmdId = core->mRequestMapping.findRequestIdAssignedToMessage(object->getId());
                app->decrementUnrespondedRequestCount(cmdId);
                if(app->getUnrespondedRequestCount(cmdId) == 0)
                {
                    switch(app->getProtocolVersion())
                    {
                        case 1:
                        {
                            NsAppLinkRPC::AddCommand_response* response = new NsAppLinkRPC::AddCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                        case 2:
                        {
                            NsAppLinkRPCV2::AddCommand_response* response = new NsAppLinkRPCV2::AddCommand_response();
                            response->set_success(true);                            
                            response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                            response->setMethodId(NsAppLinkRPCV2::FunctionID::AddCommandID);
                            response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                    }
                }

                core->mMessageMapping.removeMessage(object->getId());

                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__DELETECOMMANDRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand UI response has been income");
                NsRPC2Communication::UI::DeleteCommandResponse* object = (NsRPC2Communication::UI::DeleteCommandResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                unsigned int cmdId = core->mRequestMapping.findRequestIdAssignedToMessage(object->getId());
                app->decrementUnrespondedRequestCount(cmdId);
                if(app->getUnrespondedRequestCount(cmdId) == 0)
                {
                    switch(app->getProtocolVersion())
                    {
                        case 1:
                        {
                            NsAppLinkRPC::DeleteCommand_response* response = new NsAppLinkRPC::DeleteCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                        case 2:
                        {
                            NsAppLinkRPCV2::DeleteCommand_response* response = new NsAppLinkRPCV2::DeleteCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                    }
                }

                core->mMessageMapping.removeMessage(object->getId());

                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ADDSUBMENURESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An AddSubMenu response has been income");
                NsRPC2Communication::UI::AddSubMenuResponse* object = (NsRPC2Communication::UI::AddSubMenuResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::AddSubMenu_response* response = new NsAppLinkRPC::AddSubMenu_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::AddSubMenu_response* response = new NsAppLinkRPCV2::AddSubMenu_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__DELETESUBMENURESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A DeleteSubMenu response has been income");
                NsRPC2Communication::UI::DeleteSubMenuResponse* object = (NsRPC2Communication::UI::DeleteSubMenuResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::DeleteSubMenu_response* response = new NsAppLinkRPC::DeleteSubMenu_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::DeleteSubMenu_response* response = new NsAppLinkRPCV2::DeleteSubMenu_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__CREATEINTERACTIONCHOICESETRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A CreateInteractionChoiceSet response has been income");
                NsRPC2Communication::UI::CreateInteractionChoiceSetResponse* object = (NsRPC2Communication::UI::CreateInteractionChoiceSetResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::CreateInteractionChoiceSet_response* response = new NsAppLinkRPC::CreateInteractionChoiceSet_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::CreateInteractionChoiceSet_response* response = new NsAppLinkRPCV2::CreateInteractionChoiceSet_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->setMethodId(NsAppLinkRPCV2::FunctionID::CreateInteractionChoiceSetID);
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__DELETEINTERACTIONCHOICESETRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A DeleteInteractionChoiceSet response has been income");
                NsRPC2Communication::UI::DeleteInteractionChoiceSetResponse* object = (NsRPC2Communication::UI::DeleteInteractionChoiceSetResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPC::DeleteInteractionChoiceSet_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::DeleteInteractionChoiceSet_response* response = new NsAppLinkRPCV2::DeleteInteractionChoiceSet_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));

                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__PERFORMINTERACTIONRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A PerformInteraction response has been income");
                NsRPC2Communication::UI::PerformInteractionResponse* object = (NsRPC2Communication::UI::PerformInteractionResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::PerformInteraction_response* response = new NsAppLinkRPC::PerformInteraction_response();
                        if(object->get_choiceID())
                        {
                            response->set_choiceID(*object->get_choiceID());
                        }
                        if(object->get_triggerSource())
                        {
                            response->set_triggerSource(*object->get_triggerSource());
                        }
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::PerformInteraction_response* response = new NsAppLinkRPCV2::PerformInteraction_response();
                        if(object->get_choiceID())
                        {
                            response->set_choiceID(*object->get_choiceID());
                        }
                        if(object->get_triggerSource())
                        {
                            NsAppLinkRPCV2::TriggerSource triggerSrc;
                            triggerSrc.set((NsAppLinkRPCV2::TriggerSource::TriggerSourceInternal)object->get_triggerSource()->get());
                            response->set_triggerSource(triggerSrc);
                        }
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }

                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SETMEDIACLOCKTIMERRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A SetMediaClockTimer response has been income");
                NsRPC2Communication::UI::SetMediaClockTimerResponse* object = (NsRPC2Communication::UI::SetMediaClockTimerResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::SetMediaClockTimer_response* response = new NsAppLinkRPC::SetMediaClockTimer_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::SetMediaClockTimer_response* response = new NsAppLinkRPCV2::SetMediaClockTimer_response();
                        response->set_success(true);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        core->mMessageMapping.removeMessage(object->getId());
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ONDRIVERDISTRACTION:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnDriverDistraction UI notification has been invoked");
                NsRPC2Communication::UI::OnDriverDistraction* object = (NsRPC2Communication::UI::OnDriverDistraction*)msg;
                Application* app = AppMgrRegistry::getInstance().getActiveItem();
                if(!app)
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "No currently active application found");
                    return;
                }

                int appId = app->getAppID();

                // We need two events simultaneously, because we may have applications of more than one protocol version registered on the HMI
                // and all they need to be notified of an OnDriverDistraction event
                NsAppLinkRPC::OnDriverDistraction* eventV1 = new NsAppLinkRPC::OnDriverDistraction();
                eventV1->set_state(object->get_state());
                core->mDriverDistractionV1 = eventV1;
                NsAppLinkRPCV2::OnDriverDistraction* eventV2 = new NsAppLinkRPCV2::OnDriverDistraction();
                NsAppLinkRPCV2::DriverDistractionState stateV2;
                stateV2.set((NsAppLinkRPCV2::DriverDistractionState::DriverDistractionStateInternal)object->get_state().get());
                eventV2->set_state(stateV2);
                core->mDriverDistractionV2 = eventV2;

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        MobileHandler::getInstance().sendRPCMessage(eventV1, appId);
                        break;
                    }
                    case 2:
                    {
                        MobileHandler::getInstance().sendRPCMessage(eventV2, appId);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ONSYSTEMCONTEXT:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnSystemContext UI notification has been invoked");
                NsRPC2Communication::UI::OnSystemContext* object = (NsRPC2Communication::UI::OnSystemContext*)msg;

                Application* app = AppMgrRegistry::getInstance().getActiveItem();
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, " null-application found as an active item!");
                    return;
                }

                LOG4CPLUS_INFO_EXT(mLogger, " About to send OnHMIStatus to a mobile side...");
                int appId = app->getAppID();
                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        if (NsAppLinkRPC::SystemContext::SYSCTXT_MAIN == object->get_systemContext().get())
                        {
                            Application_v1* appv1 = (Application_v1*)app;
                            appv1->setSystemContext(object->get_systemContext());
                            NsAppLinkRPC::OnHMIStatus* event = new NsAppLinkRPC::OnHMIStatus;
                            event->set_systemContext(object->get_systemContext());
                            event->set_hmiLevel(NsAppLinkRPC::HMILevel::HMI_FULL);
                            event->set_audioStreamingState(appv1->getApplicationAudioStreamingState());

                            LOG4CPLUS_INFO_EXT(mLogger, " An NsAppLinkRPC::OnHMIStatus UI notification has been sent to a mobile side!");
                            MobileHandler::getInstance().sendRPCMessage(event, appId);
                        }
                        break;
                    }
                    case 2:
                    {
                        if (NsAppLinkRPC::SystemContext::SYSCTXT_MAIN == object->get_systemContext().get())
                        {
                            Application_v2* appv2 = (Application_v2*)app;
                            NsAppLinkRPCV2::SystemContext ctx2;
                            const NsAppLinkRPC::SystemContext& ctx = object->get_systemContext();
                            ctx2.set((NsAppLinkRPCV2::SystemContext::SystemContextInternal)ctx.get());
                            appv2->setSystemContext(ctx2);
                            NsAppLinkRPCV2::OnHMIStatus* event = new NsAppLinkRPCV2::OnHMIStatus;
                            event->set_systemContext(appv2->getSystemContext());
                            event->set_hmiLevel(NsAppLinkRPCV2::HMILevel::HMI_FULL);
                            event->set_audioStreamingState(appv2->getApplicationAudioStreamingState());

                            LOG4CPLUS_INFO_EXT(mLogger, " An NsAppLinkRPC::OnHMIStatus UI notification has been sent to a mobile side!");
                            MobileHandler::getInstance().sendRPCMessage(event, appId);
                        }
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SLIDERRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A Slider response has been income");
                NsRPC2Communication::UI::SliderResponse* uiResponse = (NsRPC2Communication::UI::SliderResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(
                    core->mMessageMapping.findRegistryItemAssignedToCommand(uiResponse->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                NsAppLinkRPCV2::Slider_response* response = new NsAppLinkRPCV2::Slider_response();

                response->set_success(true);
                response->setMethodId(NsAppLinkRPCV2::FunctionID::SliderID);
                response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                response->set_sliderPosition(uiResponse->get_sliderPosition());
                response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(uiResponse->getResult()));
                core->mMessageMapping.removeMessage(uiResponse->getId());

                LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                    << " application id " << appId);
                MobileHandler::getInstance().sendRPCMessage(response, appId);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SETAPPICONRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A SetAppId response has been income");
                NsRPC2Communication::UI::SetAppIconResponse* uiResponse = static_cast<NsRPC2Communication::UI::SetAppIconResponse*>(msg);

                Application* app = core->getApplicationFromItemCheckNotNull(
                    core->mMessageMapping.findRegistryItemAssignedToCommand(uiResponse->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                NsAppLinkRPCV2::Result::ResultInternal resultCode
                    = static_cast<NsAppLinkRPCV2::Result::ResultInternal>(uiResponse->getResult());

                NsAppLinkRPCV2::SetAppIcon_response* response = new NsAppLinkRPCV2::SetAppIcon_response();
                response->setMethodId(NsAppLinkRPCV2::FunctionID::SetAppIconID);
                response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                response->set_resultCode(resultCode);
                NsAppLinkRPCV2::Result::SUCCESS == resultCode ? response->set_success(true) : response->set_success(false);

                core->mMessageMapping.removeMessage(uiResponse->getId());
                MobileHandler::getInstance().sendRPCMessage(response, appId);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__SCROLLABLEMESSAGERESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A ScrollableMessageID response has been income");
                NsRPC2Communication::UI::ScrollableMessageResponse* uiResponse
                    = static_cast<NsRPC2Communication::UI::ScrollableMessageResponse*>(msg);

                Application* app = core->getApplicationFromItemCheckNotNull(
                    core->mMessageMapping.findRegistryItemAssignedToCommand(uiResponse->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                NsAppLinkRPCV2::Result::ResultInternal resultCode
                    = static_cast<NsAppLinkRPCV2::Result::ResultInternal>(uiResponse->getResult());

                NsAppLinkRPCV2::ScrollableMessage_response* response = new NsAppLinkRPCV2::ScrollableMessage_response();
                if (!response)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Can't create scrollable message response object");
                    return;
                }
                response->setMethodId(NsAppLinkRPCV2::FunctionID::ScrollableMessageID);
                response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                response->set_resultCode(resultCode);
                NsAppLinkRPCV2::Result::SUCCESS == resultCode ? response->set_success(true) : response->set_success(false);

                core->mMessageMapping.removeMessage(uiResponse->getId());
                MobileHandler::getInstance().sendRPCMessage(response, appId);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_UI__ONDEVICECHOSEN:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnDeviceChosen notification has been income");
                NsRPC2Communication::UI::OnDeviceChosen* chosen = (NsRPC2Communication::UI::OnDeviceChosen*)msg;
                const std::string& deviceName = chosen->get_deviceName();
                const NsConnectionHandler::CDevice* device = core->mDeviceList.findDeviceByName(deviceName);
                if (device)
                {
                    const NsConnectionHandler::tDeviceHandle& handle = device->getDeviceHandle();
                    ConnectionHandler::getInstance().connectToDevice(handle);
                }
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not UI RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VR__GETCAPABILITIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetVRCapabilities response has been income");
                NsRPC2Communication::VR::GetCapabilitiesResponse * vrCaps = (NsRPC2Communication::VR::GetCapabilitiesResponse*)msg;
                core->mVrCapabilitiesV1.set(vrCaps->get_capabilities());
                std::vector< NsAppLinkRPCV2::VrCapabilities> vrCapsV2;
                for(std::vector< NsAppLinkRPC::VrCapabilities>::const_iterator it = vrCaps->get_capabilities().begin(); it != vrCaps->get_capabilities().end(); it++)
                {
                    const NsAppLinkRPC::VrCapabilities& caps = *it;
                    NsAppLinkRPCV2::VrCapabilities capsV2;
                    capsV2.set((NsAppLinkRPCV2::VrCapabilities::VrCapabilitiesInternal)caps.get());
                    vrCapsV2.push_back(capsV2);
                }
                core->mVrCapabilitiesV2.set(vrCapsV2);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VR__GETLANGUAGERESPONSE:
            {
                NsRPC2Communication::VR::GetLanguageResponse* getLang = (NsRPC2Communication::VR::GetLanguageResponse*)msg;
                core->mVrLanguageV1 = getLang->get_language();
                NsAppLinkRPCV2::Language langV2;
                langV2.set((NsAppLinkRPCV2::Language::LanguageInternal)getLang->get_language().get());
                core->mVrLanguageV2 = langV2;
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VR__ADDCOMMANDRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An AddCommand VR response has been income");
                NsRPC2Communication::VR::AddCommandResponse* object = (NsRPC2Communication::VR::AddCommandResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                unsigned int cmdId = core->mRequestMapping.findRequestIdAssignedToMessage(object->getId());
                app->decrementUnrespondedRequestCount(cmdId);
                if(app->getUnrespondedRequestCount(cmdId) == 0)
                {
                    switch(app->getProtocolVersion())
                    {
                        case 1:
                        {
                            NsAppLinkRPC::AddCommand_response* response = new NsAppLinkRPC::AddCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                        case 2:
                        {
                            NsAppLinkRPCV2::AddCommand_response* response = new NsAppLinkRPCV2::AddCommand_response();
                            response->set_success(true);
                            response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                            response->setMethodId(NsAppLinkRPCV2::FunctionID::AddCommandID);
                            response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                    }
                }

                core->mMessageMapping.removeMessage(object->getId());

                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VR__DELETECOMMANDRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A DeleteCommand VR response has been income");
                NsRPC2Communication::VR::DeleteCommandResponse* object = (NsRPC2Communication::VR::DeleteCommandResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                unsigned int cmdId = core->mRequestMapping.findRequestIdAssignedToMessage(object->getId());
                app->decrementUnrespondedRequestCount(cmdId);
                if(app->getUnrespondedRequestCount(cmdId) == 0)
                {
                    switch(app->getProtocolVersion())
                    {
                        case 1:
                        {
                            NsAppLinkRPC::DeleteCommand_response* response = new NsAppLinkRPC::DeleteCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                        case 2:
                        {
                            NsAppLinkRPCV2::DeleteCommand_response* response = new NsAppLinkRPCV2::DeleteCommand_response();
                            response->set_success(true);
                            response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                            core->mRequestMapping.removeRequest(object->getId());
                            LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                                << " application id " << appId);
                            MobileHandler::getInstance().sendRPCMessage(response, appId);
                            break;
                        }
                    }
                }

                core->mMessageMapping.removeMessage(object->getId());

                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VR__ONCOMMAND:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " An OnCommand VR notification has been invoked");
                NsRPC2Communication::VR::OnCommand* object = (NsRPC2Communication::VR::OnCommand*)msg;
                Application* app = AppMgrRegistry::getInstance().getApplicationByCommand(object->get_cmdID());
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::OnCommand* event = new NsAppLinkRPC::OnCommand();
                        event->set_cmdID(object->get_cmdID());
                        event->set_triggerSource(NsAppLinkRPC::TriggerSource::TS_VR);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::OnCommand* event = new NsAppLinkRPCV2::OnCommand();
                        event->set_cmdID(object->get_cmdID());
                        event->set_triggerSource(NsAppLinkRPCV2::TriggerSource::TS_VR);
                        event->setMethodId(NsAppLinkRPCV2::FunctionID::OnCommandID);
                        event->setMessageType(NsAppLinkRPC::ALRPCMessage::NOTIFICATION);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(event, appId);
                        break;
                    }
                }
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not VR RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_TTS__GETCAPABILITIESRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetTTSCapabilities response has been income");
                NsRPC2Communication::TTS::GetCapabilitiesResponse * ttsCaps = (NsRPC2Communication::TTS::GetCapabilitiesResponse*)msg;
                core->mSpeechCapabilitiesV1.set(ttsCaps->get_capabilities());
                std::vector< NsAppLinkRPCV2::SpeechCapabilities> speechCapsV2;
                for(std::vector< NsAppLinkRPC::SpeechCapabilities>::const_iterator it = ttsCaps->get_capabilities().begin(); it != ttsCaps->get_capabilities().end(); it++)
                {
                    const NsAppLinkRPC::SpeechCapabilities& caps = *it;
                    NsAppLinkRPCV2::SpeechCapabilities capsV2;
                    capsV2.set((NsAppLinkRPCV2::SpeechCapabilities::SpeechCapabilitiesInternal)caps.get());
                    speechCapsV2.push_back(capsV2);
                }
                core->mSpeechCapabilitiesV2.set(speechCapsV2);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_TTS__GETLANGUAGERESPONSE:
            {
                NsRPC2Communication::TTS::GetLanguageResponse* getLang = (NsRPC2Communication::TTS::GetLanguageResponse*)msg;
                core->mTtsLanguageV1 = getLang->get_language();
                NsAppLinkRPCV2::Language langV2;
                langV2.set((NsAppLinkRPCV2::Language::LanguageInternal)getLang->get_language().get());
                core->mTtsLanguageV2 = langV2;
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_TTS__SPEAKRESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A Speak response has been income");
                NsRPC2Communication::TTS::SpeakResponse* object = (NsRPC2Communication::TTS::SpeakResponse*)msg;
                Application* app = core->getApplicationFromItemCheckNotNull(core->mMessageMapping.findRegistryItemAssignedToCommand(object->getId()));
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    return;
                }

                int appId = app->getAppID();
                core->mMessageMapping.removeMessage(object->getId());

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::Speak_response* response = new NsAppLinkRPC::Speak_response();
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->set_resultCode(static_cast<NsAppLinkRPC::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                    case 2:
                    {
                        NsAppLinkRPCV2::Speak_response* response = new NsAppLinkRPCV2::Speak_response();
                        response->setMessageType(NsAppLinkRPC::ALRPCMessage::RESPONSE);
                        response->setMethodId(NsAppLinkRPCV2::FunctionID::SpeakID);
                        response->set_resultCode(static_cast<NsAppLinkRPCV2::Result::ResultInternal>(object->getResult()));
                        response->set_success(true);
                        LOG4CPLUS_INFO_EXT(mLogger, " A message will be sent to an app " << app->getName()
                            << " application id " << appId);
                        MobileHandler::getInstance().sendRPCMessage(response, appId);
                        break;
                    }
                }
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not TTS RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_APPLINKCORE__ACTIVATEAPP:
            {
                LOG4CPLUS_INFO_EXT(mLogger, "ActivateApp has been received!");
                NsRPC2Communication::AppLinkCore::ActivateApp* object = static_cast<NsRPC2Communication::AppLinkCore::ActivateApp*>(msg);
                if ( !object )
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Couldn't cast object to ActivateApp type");
                    sendResponse<NsRPC2Communication::AppLinkCore::ActivateAppResponse,
                                NsAppLinkRPC::Result::ResultInternal>(object->getId(), NsAppLinkRPC::Result::GENERIC_ERROR);
                    return;
                }

                //  a silly workaround!!!
                //  Until the object starts supplying some sort of connection id + session id
                //  instead of just a name (there may me MORE than one app of the same name
                //  registered on HMI simultaneously).
                const std::string& appName = object->get_appName();
                AppMgrRegistry::Items items = AppMgrRegistry::getInstance().getItems(appName);
                if(items.empty())
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application with the name " << appName << " found!");
                    sendResponse<NsRPC2Communication::AppLinkCore::ActivateAppResponse,
                                NsAppLinkRPCV2::Result::ResultInternal>(object->getId(), NsAppLinkRPCV2::Result::INVALID_DATA);
                    return;
                }

                Application* app = core->getApplicationFromItemCheckNotNull(items[0]);
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
                    sendResponse<NsRPC2Communication::AppLinkCore::ActivateAppResponse,
                                NsAppLinkRPCV2::Result::ResultInternal>(object->getId(), NsAppLinkRPCV2::Result::APPLICATION_NOT_REGISTERED);
                    return;
                }

                int appId = app->getAppID();

                Application* currentApp = AppMgrRegistry::getInstance().getActiveItem();
                if (!currentApp)
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "No application is currently active");
                }
                else
                {
                    if (currentApp == app)
                    {
                        LOG4CPLUS_INFO_EXT(mLogger, "App is currently active");
                        sendResponse<NsRPC2Communication::AppLinkCore::ActivateAppResponse,
                                    NsAppLinkRPCV2::Result::ResultInternal>(object->getId(), NsAppLinkRPCV2::Result::GENERIC_ERROR);
                        return;
                    }

                    LOG4CPLUS_INFO_EXT(mLogger, "There is a currently active application  " << currentApp->getName()
                        << " ID " << currentApp->getAppID()
                        << " - about to remove it from HMI first");
                    core->removeAppFromHmi(currentApp, appId);
                }

                if(!AppMgrRegistry::getInstance().activateApp(app))
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Application " << app->getName()
                        << " application id " << appId);

                    sendResponse<NsRPC2Communication::AppLinkCore::ActivateAppResponse,
                                NsAppLinkRPCV2::Result::ResultInternal>(object->getId(), NsAppLinkRPCV2::Result::GENERIC_ERROR);
                    return;
                }

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        Application_v1* appV1 = (Application_v1*)app;
                        const ChoiceSetItems& newChoiceSets = appV1->getAllChoiceSets();
                        if(!newChoiceSets.empty())
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, "Adding new application's interaction choice sets to HMI due to a new application activation");
                            for(ChoiceSetItems::const_iterator it = newChoiceSets.begin(); it != newChoiceSets.end(); it++)
                            {
                                const unsigned int& choiceSetId = it->first;
                                const ChoiceSetV1& choiceSet = it->second.choiceSetV1;
                                NsRPC2Communication::UI::CreateInteractionChoiceSet* addCmd = new NsRPC2Communication::UI::CreateInteractionChoiceSet();
                                addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                                addCmd->set_interactionChoiceSetID(choiceSetId);
                                addCmd->set_choiceSet(choiceSet);
                                addCmd->set_appId(app->getAppID());
                                core->mMessageMapping.addMessage(addCmd->getId(), appId);

                                HMIHandler::getInstance().sendRequest(addCmd);
                            }
                            LOG4CPLUS_INFO_EXT(mLogger, "New app's interaction choice sets added!");
                        }
                        break;
                    }
                    case 2:
                    {
                        Application_v2* appV2 = (Application_v2*)app;
                        const ChoiceSetItems& newChoiceSets = appV2->getAllChoiceSets();
                        if(!newChoiceSets.empty())
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, "Adding new application's interaction choice sets to HMI due to a new application activation");
                            for(ChoiceSetItems::const_iterator it = newChoiceSets.begin(); it != newChoiceSets.end(); it++)
                            {
                                const unsigned int& choiceSetId = it->first;
                                const ChoiceSetV2& choiceSet = it->second.choiceSetV2;
                                ChoiceSetV1 choiceSetV1;
                                for(ChoiceSetV2::const_iterator it = choiceSet.begin(); it != choiceSet.end(); it++)
                                {
                                    const NsAppLinkRPCV2::Choice& choice = *it;
                                    NsAppLinkRPC::Choice choiceV1;
                                    choiceV1.set_choiceID(choice.get_choiceID());
                                    choiceV1.set_menuName(choice.get_menuName());
                                    choiceV1.set_vrCommands(choice.get_vrCommands());
                                    choiceSetV1.push_back(choiceV1);
                                }
                                NsRPC2Communication::UI::CreateInteractionChoiceSet* addCmd = new NsRPC2Communication::UI::CreateInteractionChoiceSet();
                                addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                                addCmd->set_interactionChoiceSetID(choiceSetId);
                                addCmd->set_choiceSet(choiceSetV1);
                                addCmd->set_appId(app->getAppID());
                                core->mMessageMapping.addMessage(addCmd->getId(), appId);

                                HMIHandler::getInstance().sendRequest(addCmd);
                            }
                            LOG4CPLUS_INFO_EXT(mLogger, "New app's interaction choice sets added!");
                        }
                        break;
                    }
                }

                const MenuItems& newMenus = app->getAllMenus();
                if(!newMenus.empty())
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "Adding new application's menus to HMI due to a new application activation");
                    for(MenuItems::const_iterator it = newMenus.begin(); it != newMenus.end(); it++)
                    {
                        const unsigned int& menuId = it->first;
                        const MenuValue& menuVal = it->second;
                        const std::string& menuName = menuVal.first;
                        const unsigned int* position = menuVal.second;
                        NsRPC2Communication::UI::AddSubMenu* addCmd = new NsRPC2Communication::UI::AddSubMenu();
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        addCmd->set_menuId(menuId);
                        addCmd->set_menuName(menuName);
                        if(position)
                        {
                            addCmd->set_position(*position);
                        }
                        addCmd->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(addCmd->getId(), appId);

                        HMIHandler::getInstance().sendRequest(addCmd);
                    }
                    LOG4CPLUS_INFO_EXT(mLogger, "New app's menus added!");
                }

                const Commands& newCommands = app->getAllCommands();
                if(!newCommands.empty())
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "Adding a new application's commands to HMI due to a new application activation");
                    for(Commands::const_iterator it = newCommands.begin(); it != newCommands.end(); it++)
                    {
                        const Command& key = *it;
                        const CommandParams& params = key.second;
                        const NsAppLinkRPC::MenuParams* menuParams = params.menuParams;
                        const std::vector<std::string>* vrCommands = params.vrCommands;
                        const CommandBase& base = key.first;
                        const CommandType& type = std::get<1>(base);
                        unsigned int cmdId = std::get<0>(base);

                        NsRPC2Communication::RPC2Request* addCmd = 0;
                        if(type == CommandType::UI)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, "Adding UI command");
                            addCmd = new NsRPC2Communication::UI::AddCommand();
                            ((NsRPC2Communication::UI::AddCommand*)addCmd)->set_menuParams(*menuParams);
                        }
                        else if(type == CommandType::VR)
                        {
                            LOG4CPLUS_INFO_EXT(mLogger, "Adding VR command");
                            addCmd = new NsRPC2Communication::VR::AddCommand();
                            ((NsRPC2Communication::VR::AddCommand*)addCmd)->set_vrCommands(*vrCommands);
                        }
                        else
                        {
                            LOG4CPLUS_ERROR_EXT(mLogger, "An unindentified command type - " << type.getType());
                            continue;
                        }
                        addCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                        ((NsRPC2Communication::UI::AddCommand*)addCmd)->set_cmdId(cmdId); //doesn't matter, of which type- VR or UI is thye cmd = eather has the set_cmdId method within
                        ((NsRPC2Communication::UI::AddCommand*)addCmd)->set_appId(app->getAppID());
                        core->mMessageMapping.addMessage(addCmd->getId(), appId);

                        HMIHandler::getInstance().sendRequest(addCmd);
                    }
                    LOG4CPLUS_INFO_EXT(mLogger, "New app's commands added!");
                }

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        Application_v1* appv1 = (Application_v1*)app;
                        NsAppLinkRPC::OnHMIStatus * hmiStatus = new NsAppLinkRPC::OnHMIStatus;
                        hmiStatus->set_hmiLevel(NsAppLinkRPC::HMILevel::HMI_FULL);
                        if ( appv1->getIsMediaApplication() )
                        {
                            appv1->setApplicationAudioStreamingState(NsAppLinkRPC::AudioStreamingState::AUDIBLE);
                        }
                        else
                        {
                            appv1->setApplicationAudioStreamingState(NsAppLinkRPC::AudioStreamingState::NOT_AUDIBLE);
                        }
                        hmiStatus->set_audioStreamingState(appv1->getApplicationAudioStreamingState());
                        hmiStatus->set_systemContext(appv1->getSystemContext());
                        MobileHandler::getInstance().sendRPCMessage( hmiStatus, appId );
                        NsRPC2Communication::AppLinkCore::ActivateAppResponse * response = new NsRPC2Communication::AppLinkCore::ActivateAppResponse;
                        response->setId(object->getId());
                        response->setResult(NsAppLinkRPC::Result::SUCCESS);
                        HMIHandler::getInstance().sendResponse(response);

                        if(core->mDriverDistractionV1)
                        {
                            MobileHandler::getInstance().sendRPCMessage(core->mDriverDistractionV1, appId);
                        }

                        break;
                    }
                    case 2:
                    {
                        Application_v2* appv2 = (Application_v2*)app;
                        NsAppLinkRPCV2::OnHMIStatus * hmiStatus = new NsAppLinkRPCV2::OnHMIStatus;
                        hmiStatus->set_hmiLevel(NsAppLinkRPCV2::HMILevel::HMI_FULL);
                        if ( appv2->getIsMediaApplication() )
                        {
                            appv2->setApplicationAudioStreamingState(NsAppLinkRPCV2::AudioStreamingState::AUDIBLE);
                        }
                        else
                        {
                            appv2->setApplicationAudioStreamingState(NsAppLinkRPCV2::AudioStreamingState::NOT_AUDIBLE);
                        }
                        hmiStatus->set_audioStreamingState(appv2->getApplicationAudioStreamingState());
                        hmiStatus->set_systemContext(appv2->getSystemContext());
                        MobileHandler::getInstance().sendRPCMessage( hmiStatus, appId );
                        NsRPC2Communication::AppLinkCore::ActivateAppResponse * response = new NsRPC2Communication::AppLinkCore::ActivateAppResponse;
                        response->setId(object->getId());
                        response->setResult(NsAppLinkRPCV2::Result::SUCCESS);
                        HMIHandler::getInstance().sendResponse(response);

                        if(core->mDriverDistractionV2)
                        {
                            MobileHandler::getInstance().sendRPCMessage(core->mDriverDistractionV2, appId);
                        }

                        break;
                    }
                }
                LOG4CPLUS_INFO_EXT(mLogger, "New app  " << app->getName() << " id " << app->getAppID() << " activated!");
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_APPLINKCORE__DEACTIVATEAPP:
            {
                LOG4CPLUS_INFO_EXT(mLogger, "DeactivateApp has been received!");
                NsRPC2Communication::AppLinkCore::DeactivateApp* object = static_cast<NsRPC2Communication::AppLinkCore::DeactivateApp*>(msg);
                if ( !object )
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Couldn't cast object to DeactivateApp type");
                    return;
                }

                Application* currentApp = AppMgrRegistry::getInstance().getActiveItem();
                if (!currentApp)
                {
                    LOG4CPLUS_INFO_EXT(mLogger, "No application is currently active");
                    return;
                }

                /*switch(currentApp->getApplicationHMIStatusLevel())
                {
                    case NsAppLinkRPC::HMILevel::HMI_FULL:
                    break;
                    case NsAppLinkRPC::HMILevel::HMI_LIMITED:
                    break;
                    case NsAppLinkRPC::HMILevel::HMI_BACKGROUND:
                    break;
                    case NsAppLinkRPC::HMILevel::HMI_NONE:
                    break;
                }*/

                break;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_APPLINKCORE__SENDDATA:
            {
                LOG4CPLUS_INFO_EXT(mLogger, "SendData request has been received!");
                NsRPC2Communication::AppLinkCore::SendData* object = static_cast<NsRPC2Communication::AppLinkCore::SendData*>(msg);
                core->mSyncPManager.setRawData( object->get_data() );
                Application* app = AppMgrRegistry::getInstance().getActiveItem();
                if(!app)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, " No active application found!");
                    return;
                }

                int appId = app->getAppID();

                switch(app->getProtocolVersion())
                {
                    case 1:
                    {
                        NsAppLinkRPC::OnEncodedSyncPData* encodedNotification = new NsAppLinkRPC::OnEncodedSyncPData;
                        encodedNotification->set_data(core->mSyncPManager.getPData());
                        MobileHandler::getInstance().sendRPCMessage( encodedNotification, appId );
                        NsRPC2Communication::AppLinkCore::SendDataResponse* response = new NsRPC2Communication::AppLinkCore::SendDataResponse;
                        response->setId(object->getId());
                        response->setResult(NsAppLinkRPC::Result::SUCCESS);
                        HMIHandler::getInstance().sendResponse(response);
                        break;
                    }
                    case 2:
                    {
                        NsRPC2Communication::AppLinkCore::SendDataResponse* response = new NsRPC2Communication::AppLinkCore::SendDataResponse;
                        response->setId(object->getId());
                        const std::string* urlPtr = object->get_url();
                        const int* timeoutPtr = object->get_timeout();
                        if(urlPtr)
                        {
                            const std::string& url = *urlPtr;
                            const int& timeout = timeoutPtr ? *timeoutPtr : 0;
                            LOG4CPLUS_INFO_EXT(mLogger, "SendData about to send at " << url << " timeout " << timeout);
                            pthread_t* sendingThread = 0;
                            thread_data* data = new thread_data;
                            data->pdata = core->mSyncPManager.getPData();
                            data->timeout = timeout;
                            data->url = url;
                            int rc = pthread_create(sendingThread, 0, SendPData,
                                           (void *) data);
                            if (rc)
                            {
                                 LOG4CPLUS_ERROR_EXT(mLogger, "Couldn't start a thread: return code from pthread_create() is " << rc);
                                 response->setResult(NsAppLinkRPCV2::Result::GENERIC_ERROR);
                                 HMIHandler::getInstance().sendResponse(response);
                                 return;
                            }
                            LOG4CPLUS_INFO_EXT(mLogger, "Data sending thread started!");
                        }
                        else
                        {
                            NsAppLinkRPCV2::OnEncodedSyncPData* encodedNotification = new NsAppLinkRPCV2::OnEncodedSyncPData;
                            encodedNotification->set_data(core->mSyncPManager.getPData());
                            MobileHandler::getInstance().sendRPCMessage( encodedNotification, appId );
                        }
                        response->setResult(NsAppLinkRPCV2::Result::SUCCESS);
                        HMIHandler::getInstance().sendResponse(response);
                        break;
                    }
                }
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_APPLINKCORE__GETAPPLIST:
            {
                LOG4CPLUS_INFO_EXT(mLogger, "GetAppList request has been received!");
                NsRPC2Communication::AppLinkCore::GetAppList* object = static_cast<NsRPC2Communication::AppLinkCore::GetAppList*>(msg);
                NsRPC2Communication::AppLinkCore::GetAppListResponse* response = new NsRPC2Communication::AppLinkCore::GetAppListResponse;
                response->setId(object->getId());
                const AppMgrRegistry::ItemsMap& registeredApps = AppMgrRegistry::getInstance().getItems();
                std::vector< NsAppLinkRPC::HMIApplication> hmiApps;
                for(AppMgrRegistry::ItemsMap::const_iterator it = registeredApps.begin(); it != registeredApps.end(); it++)
                {
                    NsAppLinkRPC::HMIApplication hmiApp;
                    Application* app = core->getApplicationFromItemCheckNotNull(it->second);
                    if(!app)
                    {
                        LOG4CPLUS_ERROR_EXT(mLogger, " null-application found!");
                        continue;
                    }

                    LOG4CPLUS_INFO_EXT(mLogger, "Adding an application " << app->getName()
                        << " application id " << app->getAppID()
                        << " is media? " << app->getIsMediaApplication() );

                    hmiApp.set_appName(app->getName());
                    hmiApp.set_ngnMediaScreenAppName(app->getNgnMediaScreenAppName());
                    hmiApp.set_appId(app->getAppID());
                    hmiApp.set_isMediaApplication(app->getIsMediaApplication());
                    
                    LOG4CPLUS_INFO_EXT(mLogger, "Added an application " << hmiApp.get_appName()
                        << " application id " << hmiApp.get_appId()
                        << " is media? " << hmiApp.get_isMediaApplication() );
                    hmiApps.push_back(hmiApp);
                }
                if(!hmiApps.empty())
                {
                    response->set_appList(hmiApps);
                    response->setResult(NsAppLinkRPC::Result::SUCCESS);
                }
                else
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, " Application list is empty!");
                    response->setResult(NsAppLinkRPC::Result::GENERIC_ERROR);
                }

                Json::Value commandJson = NsRPC2Communication::Marshaller::toJSON( response );
                LOG4CPLUS_INFO(mLogger, "JSONRPC2Handler::waitForCommandsToHMI: received command text: " << commandJson);
                HMIHandler::getInstance().sendResponse(response);
                return;
            }
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_APPLINKCORE__GETDEVICELIST:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetDeviceList request has been income");
                NsRPC2Communication::AppLinkCore::GetDeviceList* getDevList = (NsRPC2Communication::AppLinkCore::GetDeviceList*)msg;
                NsRPC2Communication::AppLinkCore::GetDeviceListResponse* response = new NsRPC2Communication::AppLinkCore::GetDeviceListResponse;
                response->setId(getDevList->getId());
                DeviceNamesList list;
                const NsConnectionHandler::tDeviceList& devList = core->mDeviceList.getDeviceList();
                for(NsConnectionHandler::tDeviceList::const_iterator it = devList.begin(); it != devList.end(); it++)
                {
                    const NsConnectionHandler::CDevice& device = it->second;
                    list.push_back(device.getUserFriendlyName());
                }
                if ( list.empty() )
                {
                    list.push_back("");
                    response->setResult(NsAppLinkRPC::Result::GENERIC_ERROR);
                }
                else
                {
                    response->setResult(NsAppLinkRPC::Result::SUCCESS);
                }
                response->set_deviceList(list);
                ConnectionHandler::getInstance().startDevicesDiscovery();
                HMIHandler::getInstance().sendResponse(response);
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not AppLinkCore RPC message " << msg->getMethod() << " has been received!");
        }

        switch(msg->getMethod())
        {
            case NsRPC2Communication::Marshaller::METHOD_NSRPC2COMMUNICATION_VEHICLEINFO__GETVEHICLETYPERESPONSE:
            {
                LOG4CPLUS_INFO_EXT(mLogger, " A GetVehicleType response has been income");
                NsRPC2Communication::VehicleInfo::GetVehicleTypeResponse* getVehType = (NsRPC2Communication::VehicleInfo::GetVehicleTypeResponse*)msg;
                core->mVehicleType = getVehType->get_vehicleType();
                return;
            }
            default:
                LOG4CPLUS_INFO_EXT(mLogger, " Not VehicleInfo RPC message " << msg->getMethod() << " has been received!");
        }
        LOG4CPLUS_INFO_EXT(mLogger, " A RPC2 bus message " << msg->getMethod() << " has been invoked!");
    }

    /**
     * \brief Register an application
     * \param request a RegisterAppInterface request which is the source for application fields initial values
     * \param connectionID id of the connection which will be associated with the application
     * \param sessionID an id of the session which will be associated with the application
     * \return A instance of RegistryItem created for application
     */
    const RegistryItem* AppMgrCore::registerApplication(NsAppLinkRPC::ALRPCMessage * request, int appId)
    {
        LOG4CPLUS_INFO_EXT(mLogger, __PRETTY_FUNCTION__);
        if(!request)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "Null-request specified for application id " << appId << "!");
            return 0;
        }

        const unsigned int& protocolVersion = request->getProtocolVersion();
        std::string appName = "";

        switch(protocolVersion)
        {
            case 2:
            {
                appName = ((NsAppLinkRPCV2::RegisterAppInterface_request*)request)->get_appName();
                Application_v2* application = new Application_v2(appName, appId);
                if(!application)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Cannot register application " << appName
                        << " application id " << appId
                        << " protocol version " << protocolVersion
                        << " !");
                    return 0;
                }

                NsAppLinkRPCV2::RegisterAppInterface_request* registerRequest = (NsAppLinkRPCV2::RegisterAppInterface_request*) request;
       /*         if(registerRequest->get_appID())
                {
                    const std::string& appId = *registerRequest->get_appID();
                    application->setAppID(appId);
                }
        */        if( registerRequest->get_appType() )
                {
                    const std::vector<NsAppLinkRPCV2::AppType>& appType = *registerRequest->get_appType();
                    application->setAppType(appType);
                }
                bool isMediaApplication = registerRequest->get_isMediaApplication();
                const NsAppLinkRPCV2::Language& languageDesired = registerRequest->get_languageDesired();
                application->setLanguageDesired(languageDesired);

                const NsAppLinkRPCV2::SyncMsgVersion& syncMsgVersion = registerRequest->get_syncMsgVersion();

                if ( registerRequest -> get_ngnMediaScreenAppName() )
                {
                    const std::string& ngnMediaScreenAppName = *registerRequest->get_ngnMediaScreenAppName();
                    application->setNgnMediaScreenAppName(ngnMediaScreenAppName);
                }

                if ( registerRequest -> get_vrSynonyms() )
                {
                    const std::vector<std::string>& vrSynonyms = *registerRequest->get_vrSynonyms();
                    application->setVrSynonyms(vrSynonyms);
                }

                application->setHMIDisplayLanguageDesired(registerRequest->get_hmiDisplayLanguageDesired());
                application->setIsMediaApplication(isMediaApplication);
                application->setSyncMsgVersion(syncMsgVersion);
                application->setSystemContext(NsAppLinkRPCV2::SystemContext::SYSCTXT_MAIN);

                if(registerRequest->get_ttsName())
                {
                    application->setTtsName(*registerRequest->get_ttsName());
                }

                application->setApplicationHMIStatusLevel(NsAppLinkRPCV2::HMILevel::HMI_NONE);

                return AppMgrRegistry::getInstance().registerApplication( application );
            }
            case 1:
            {
                appName = ((NsAppLinkRPC::RegisterAppInterface_request*)request)->get_appName();
                Application_v1* application = new Application_v1(appName, appId);
                if(!application)
                {
                    LOG4CPLUS_ERROR_EXT(mLogger, "Cannot register application " << appName
                        << " application id " << appId
                        << " protocol version " << protocolVersion
                        << " !");
                    return 0;
                }

                NsAppLinkRPC::RegisterAppInterface_request* registerRequest = (NsAppLinkRPC::RegisterAppInterface_request*) request;
                bool isMediaApplication = registerRequest->get_isMediaApplication();
                const NsAppLinkRPC::Language& languageDesired = registerRequest->get_languageDesired();
                const NsAppLinkRPC::SyncMsgVersion& syncMsgVersion = registerRequest->get_syncMsgVersion();

                if ( registerRequest -> get_ngnMediaScreenAppName() )
                {
                    const std::string& ngnMediaScreenAppName = *registerRequest->get_ngnMediaScreenAppName();
                    application->setNgnMediaScreenAppName(ngnMediaScreenAppName);
                }

                if ( registerRequest -> get_vrSynonyms() )
                {
                    const std::vector<std::string>& vrSynonyms = *registerRequest->get_vrSynonyms();
                    application->setVrSynonyms(vrSynonyms);
                }

                if ( registerRequest -> get_usesVehicleData() )
                {
                    bool usesVehicleData = registerRequest->get_usesVehicleData();
                    application->setUsesVehicleData(usesVehicleData);
                }

                application->setIsMediaApplication(isMediaApplication);
                application->setLanguageDesired(languageDesired);
                application->setSyncMsgVersion(syncMsgVersion);
                application->setSystemContext(NsAppLinkRPC::SystemContext::SYSCTXT_MAIN);

                application->setApplicationHMIStatusLevel(NsAppLinkRPC::HMILevel::HMI_NONE);

                LOG4CPLUS_INFO_EXT(mLogger, "Application created." );
                return AppMgrRegistry::getInstance().registerApplication( application );
            }
            default:
            {
                LOG4CPLUS_ERROR_EXT(mLogger, "Unsupported protocol version number " << protocolVersion << " !");
                return 0;
            }
        }
        LOG4CPLUS_INFO_EXT(mLogger, " Application " << appName << " application id " << appId << " registered successfully !");
        return 0;
    }

    /**
     * \brief unregister an application associated with the given session
     * \param appId an id of the application to be unregistered
     */
    void AppMgrCore::unregisterApplication(int appId)
    {
        LOG4CPLUS_INFO_EXT(mLogger, "Trying to unregister an application for application id " << appId);
        RegistryItem* item = AppMgrRegistry::getInstance().getItem(appId);
        Application* app = getApplicationFromItemCheckNotNull( item );
        if(!app)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
            return;
        }

        const std::string& appName = app->getName();
        LOG4CPLUS_INFO_EXT(mLogger, " Unregistering an application " << appName
            << " application id " << appId
            << "!");

        mButtonsMapping.removeItem(item);
        mMessageMapping.removeItem(item);
        AppMgrRegistry::getInstance().unregisterApplication(item);

        LOG4CPLUS_INFO_EXT(mLogger, " Unregistered an application " << appName
            << " application id " << appId
            << "!");
    }

    /**
     * \brief Remove all app components from HMI
     * \param currentApp app which components to be removed
     * \param appId application id
     */
    void AppMgrCore::removeAppFromHmi(Application* currentApp, int appId)
    {
        const Commands& currentCommands = currentApp->getAllCommands();
        LOG4CPLUS_INFO_EXT(mLogger, "Removing current application's commands from HMI");

        if (1 == currentApp->getProtocolVersion())
        {
            NsAppLinkRPC::OnHMIStatus* hmiStatus = new NsAppLinkRPC::OnHMIStatus;
            NsAppManager::Application_v1* currentAppV1 = static_cast<NsAppManager::Application_v1*>(currentApp);
            currentAppV1->setApplicationHMIStatusLevel(NsAppLinkRPC::HMILevel::HMI_BACKGROUND);
            hmiStatus->set_audioStreamingState(currentAppV1->getApplicationAudioStreamingState());
            hmiStatus->set_systemContext(currentAppV1->getSystemContext());
            hmiStatus->set_hmiLevel(NsAppLinkRPC::HMILevel::HMI_BACKGROUND);
            MobileHandler::getInstance().sendRPCMessage(hmiStatus, appId);
        }
        else
        {
            NsAppLinkRPCV2::OnHMIStatus* hmiStatus = new NsAppLinkRPCV2::OnHMIStatus;
            NsAppManager::Application_v2* currentAppV2 = static_cast<NsAppManager::Application_v2*>(currentApp);
            currentAppV2->setApplicationHMIStatusLevel(NsAppLinkRPCV2::HMILevel::HMI_BACKGROUND);
            hmiStatus->set_audioStreamingState(currentAppV2->getApplicationAudioStreamingState());
            hmiStatus->set_systemContext(currentAppV2->getSystemContext());
            hmiStatus->set_hmiLevel(NsAppLinkRPCV2::HMILevel::HMI_BACKGROUND);
            MobileHandler::getInstance().sendRPCMessage(hmiStatus, appId);
        }

        for(Commands::const_iterator it = currentCommands.begin(); it != currentCommands.end(); it++)
        {
            const Command& key = *it;
            const CommandParams& params = key.second;
            const CommandBase& base = key.first;
            const CommandType& type = std::get<1>(base);
            unsigned int cmdId = std::get<0>(base);
            NsRPC2Communication::RPC2Request* deleteCmd = 0;
            if(type == CommandType::UI)
            {
                LOG4CPLUS_INFO_EXT(mLogger, "Removing UI command");
                deleteCmd = new NsRPC2Communication::UI::DeleteCommand();
            }
            else if(type == CommandType::VR)
            {
                LOG4CPLUS_INFO_EXT(mLogger, "Removing VR command");
                deleteCmd = new NsRPC2Communication::VR::DeleteCommand();
            }
            else
            {
                LOG4CPLUS_ERROR_EXT(mLogger, "An unindentified command type - " << type.getType());
                continue;
            }
            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
            ((NsRPC2Communication::UI::DeleteCommand*)deleteCmd)->set_cmdId(cmdId); //doesn't matter, of which type- VR or UI is thye cmd = eather has the set_cmdId method within
            ((NsRPC2Communication::UI::DeleteCommand*)deleteCmd)->set_appId(currentApp->getAppID());
            mMessageMapping.addMessage(deleteCmd->getId(), appId);

            HMIHandler::getInstance().sendRequest(deleteCmd);
        }
        LOG4CPLUS_INFO_EXT(mLogger, "Current app's commands removed!");

        const MenuItems& currentMenus = currentApp->getAllMenus();
        LOG4CPLUS_INFO_EXT(mLogger, "Removing current application's menus from HMI");
        for(MenuItems::const_iterator it = currentMenus.begin(); it != currentMenus.end(); it++)
        {
            const unsigned int& menuId = it->first;
            NsRPC2Communication::UI::DeleteSubMenu* deleteCmd = new NsRPC2Communication::UI::DeleteSubMenu();
            deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
            deleteCmd->set_menuId(menuId);
            deleteCmd->set_appId(currentApp->getAppID());
            mMessageMapping.addMessage(deleteCmd->getId(), appId);

            HMIHandler::getInstance().sendRequest(deleteCmd);
        }
        LOG4CPLUS_INFO_EXT(mLogger, "Current app's menus removed!");

        switch(currentApp->getProtocolVersion())
        {
            case 1:
            {
                Application_v1* appV1 = (Application_v1*)currentApp;
                const ChoiceSetItems& currentChoiceSets = appV1->getAllChoiceSets();
                LOG4CPLUS_INFO_EXT(mLogger, "Removing current application's interaction choice sets from HMI");
                for(ChoiceSetItems::const_iterator it = currentChoiceSets.begin(); it != currentChoiceSets.end(); it++)
                {
                    const unsigned int& choiceSetId = it->first;
                    NsRPC2Communication::UI::DeleteInteractionChoiceSet* deleteCmd = new NsRPC2Communication::UI::DeleteInteractionChoiceSet();
                    deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    deleteCmd->set_interactionChoiceSetID(choiceSetId);
                    deleteCmd->set_appId(currentApp->getAppID());
                    mMessageMapping.addMessage(deleteCmd->getId(), appId);
                    HMIHandler::getInstance().sendRequest(deleteCmd);
                }
                LOG4CPLUS_INFO_EXT(mLogger, "Current app's interaction choice sets removed!");
                break;
            }
            case 2:
            {
                Application_v2* appV2 = (Application_v2*)currentApp;
                const ChoiceSetItems& currentChoiceSets = appV2->getAllChoiceSets();
                LOG4CPLUS_INFO_EXT(mLogger, "Removing current application's interaction choice sets from HMI");
                for(ChoiceSetItems::const_iterator it = currentChoiceSets.begin(); it != currentChoiceSets.end(); it++)
                {
                    const unsigned int& choiceSetId = it->first;
                    NsRPC2Communication::UI::DeleteInteractionChoiceSet* deleteCmd = new NsRPC2Communication::UI::DeleteInteractionChoiceSet();
                    deleteCmd->setId(HMIHandler::getInstance().getJsonRPC2Handler()->getNextMessageId());
                    deleteCmd->set_interactionChoiceSetID(choiceSetId);
                    deleteCmd->set_appId(currentApp->getAppID());
                    mMessageMapping.addMessage(deleteCmd->getId(), appId);
                    HMIHandler::getInstance().sendRequest(deleteCmd);
                }
                LOG4CPLUS_INFO_EXT(mLogger, "Current app's interaction choice sets removed!");
                break;
            }
        }
    }

    /**
     * \brief retrieve an application instance from the RegistryItrem instance checking for non-null values
     * \param item a RegistryItem from which to retrieve an app pointer
     * \return Application instance retrieved from item
     */
    Application *AppMgrCore::getApplicationFromItemCheckNotNull(const RegistryItem *item) const
    {
        if(!item)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "No registry item found!");
            return 0;
        }
        Application* app = item->getApplication();
        if(!app)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "No application associated with this registry item!");
            return 0;
        }
        return app;
    }

    /**
     * \brief serialize a string value to the text file
     * \param fileName name of the file to serialize to
     * \param value a value to serialize
     * \return success of an operation - true or false
     */
    bool AppMgrCore::serializeToFile(const std::string &fileName, const std::string& value) const
    {
        if(!value.empty())
        {
            std::ofstream file(fileName, std::ios::out | std::ios::trunc);
            if(file.is_open())
            {
                file << value;
                file.close();
                LOG4CPLUS_INFO_EXT(mLogger, " Serialized a value " << value << " to a file " << fileName);
                return true;
            }
            else
            {
                LOG4CPLUS_INFO_EXT(mLogger, " AppMgrCore cannot serialize a value " << value << " to a file " << fileName << ": error creating file!");
                return false;
            }
        }
        LOG4CPLUS_ERROR_EXT(mLogger, " Cannot serialize an empty value to a file " << fileName << " !");
        return false;
    }

    /**
     * \brief set Json mobile handler
     * \param handler a handler instance
     */
    void AppMgrCore::setJsonHandler(JSONHandler* handler)
    {
        if(!handler)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "A null pointer is being assigned - is this the intent?");
            return;
        }
        MobileHandler::getInstance().setJsonHandler(handler);
    }

    /**
     * \brief get Json mobile handler
     * \return JSONHandler instance
     */
    JSONHandler* AppMgrCore::getJsonHandler( ) const
    {
        return MobileHandler::getInstance().getJsonHandler();
    }

    /**
     * \brief set Json RPC2 handler
     * \param handler a handler instance
     */
    void AppMgrCore::setJsonRPC2Handler(JSONRPC2Handler *handler)
    {
        if(!handler)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "A null pointer is being assigned - is this the intent?");
            return;
        }
        HMIHandler::getInstance().setJsonRPC2Handler(handler);
    }

    /**
     * \brief get Json RPC2 handler
     * \return JSONRPC2Handler instance
     */
    JSONRPC2Handler *AppMgrCore::getJsonRPC2Handler() const
    {
        return HMIHandler::getInstance().getJsonRPC2Handler();
    }

    /**
     * \brief Sets connection handler instance
     * \param handler connection handler
     */
    void AppMgrCore::setConnectionHandler(NsConnectionHandler::IDevicesDiscoveryStarter *handler)
    {
        if(!handler)
        {
            LOG4CPLUS_ERROR_EXT(mLogger, "A null pointer is being assigned - is this the intent?");
            return;
        }
        ConnectionHandler::getInstance().setConnectionHandler(handler);
    }

    /**
     * \brief Gets connection handler instance
     * \return connection handler
     */
    NsConnectionHandler::IDevicesDiscoveryStarter *AppMgrCore::getConnectionHandler() const
    {
        return ConnectionHandler::getInstance().getConnectionHandler();
    }

    /**
     * \brief set device list
     * \param deviceList device list
     */
    void AppMgrCore::setDeviceList(const NsConnectionHandler::tDeviceList &deviceList)
    {
        LOG4CPLUS_INFO_EXT(mLogger, " Updating device list: " << deviceList.size() << " devices");
        mDeviceList.setDeviceList(deviceList);
        NsRPC2Communication::AppLinkCore::OnDeviceListUpdated* deviceListUpdated = new NsRPC2Communication::AppLinkCore::OnDeviceListUpdated;
        DeviceNamesList list;
        const NsConnectionHandler::tDeviceList& devList = mDeviceList.getDeviceList();
        for(NsConnectionHandler::tDeviceList::const_iterator it = devList.begin(); it != devList.end(); it++)
        {
            const NsConnectionHandler::CDevice& device = it->second;
            list.push_back(device.getUserFriendlyName());
        }
        if ( list.empty() )
        {
            list.push_back("");
        }

        deviceListUpdated->set_deviceList(list);
        HMIHandler::getInstance().sendNotification(deviceListUpdated);
    }

    /**
     * \brief get device list
     * \return device list
     */
    const NsConnectionHandler::tDeviceList &AppMgrCore::getDeviceList() const
    {
        return mDeviceList.getDeviceList();
    }

    /**
     * \brief add a device to a mapping
     * \param sessionKey session/connection key
     * \param device device handler
     */
    void AppMgrCore::addDevice(const int &sessionKey, const NsConnectionHandler::tDeviceHandle &device)
    {
        mDeviceHandler.addDevice(sessionKey, device);
    }

    /**
     * \brief remove a device from a mapping
     * \param sessionKey session/connection key
     */
    void AppMgrCore::removeDevice(const int &sessionKey)
    {
        mDeviceHandler.removeDevice(sessionKey);
    }

}
