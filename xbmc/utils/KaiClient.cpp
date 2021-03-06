/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
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
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "KaiClient.h"
#include "KaiRequestList.h"
#include "GUISettings.h"
#ifdef HAS_KAI_VOICE
#include "dsstdfx.h"
#endif

CKaiClient* CKaiClient::client = NULL;

//#define SPEEX_LOOPBACK 1
#define KAI_CONTACT_SETTLING_PERIOD  5000L
#define KAI_REACHABLE_QUERY_PERIOD  15000L

#ifdef SPEEX_LOOPBACK
DWORD g_speexLoopbackPlayerId = 0xDEADBEEF;
#endif


CKaiClient::CKaiClient(void) : CUdpClient()
{
  CLog::Log(LOGINFO, "KAICLIENT: Instantiating...");
  observer = NULL;
  m_bHeadset = FALSE;
  m_bHosting = FALSE;
  m_bContactsSettling = TRUE;
  m_bReachable = FALSE;
  m_nFriendsOnline = 0;
#ifdef HAS_KAI_VOICE
  m_pDSound = NULL;

  // outbound packet queue, collects compressed audio until sufficient to send
  CStdString strEgress = "egress";
  m_pEgress = new CMediaPacketQueue(strEgress);
#endif

  // request list
  m_pRequestList = new CKaiRequestList();

  Create();

  CLog::Log(LOGINFO, "KAICLIENT: Ready.");
}


void CKaiClient::QueryVector(DWORD aTitleId)
{
  CStdString strResolveMessage;
  CStdString strQuestion;
  strQuestion.Format("%x", aTitleId);
  strResolveMessage.Format("KAI_CLIENT_APP_SPECIFIC;XBE_0x%s;", strQuestion.ToUpper().c_str());

  m_pRequestList->QueueRequest(strResolveMessage);
}

void CKaiClient::QueryVectorPlayerCount(CStdString& aVector)
{
  CStdString strQueryMessage;
  strQueryMessage.Format("KAI_CLIENT_SPECIFIC_COUNT;%s;", aVector.c_str());

  m_pRequestList->QueueRequest(strQueryMessage);
}

void CKaiClient::VoiceChatStart()
{
#ifdef HAS_KAI_VOICE
  if (!m_pDSound)
  {
    CLog::Log(LOGINFO, "KAICLIENT: Initializing DirectSound...");
    if (FAILED(DirectSoundCreate( NULL, &m_pDSound, NULL )))
    {
      CLog::Log(LOGERROR, "KAICLIENT: Failed to initialize DirectSound.");
    }

    DSEFFECTIMAGELOC dsImageLoc;
    dsImageLoc.dwI3DL2ReverbIndex = GraphI3DL2_I3DL2Reverb;
    dsImageLoc.dwCrosstalkIndex = GraphXTalk_XTalk;

    CLog::Log(LOGINFO, "KAICLIENT: Loading sound effects image...");
    LPDSEFFECTIMAGEDESC pdsImageDesc;
    if (FAILED(XAudioDownloadEffectsImage( "dsstdfx", &dsImageLoc, XAUDIO_DOWNLOADFX_XBESECTION, &pdsImageDesc )))
    {
      CLog::Log(LOGERROR, "KAICLIENT: Failed to load image.");
    }

    // Configure the voice manager.
    VOICE_MANAGER_CONFIG VoiceConfig;
    VoiceConfig.dwVoicePacketTime = 20;      // 20ms per microphone callback
    VoiceConfig.dwMaxRemotePlayers = 12;      // 12 remote players
    VoiceConfig.dwFirstSRCEffectIndex = GraphVoice_Voice_0;
    VoiceConfig.pEffectImageDesc = pdsImageDesc;
    VoiceConfig.pDSound = m_pDSound;
    VoiceConfig.dwMaxStoredPackets = MAX_VOICE_PER_SEND;

    VoiceConfig.pCallbackContext = this;
    VoiceConfig.pfnCommunicatorCallback = CommunicatorCallback;
    VoiceConfig.pfnVoiceDataCallback = VoiceDataCallback;

    CLog::Log(LOGNOTICE, "KAICLIENT: Initializing voice manager...");
    if (FAILED(g_VoiceManager.Initialize( &VoiceConfig )))
    {
      CLog::Log(LOGERROR, "KAICLIENT: Failed to initialize voice manager.");
    }

    CLog::Log(LOGINFO, "KAICLIENT: Voice chat enabled.");

    g_VoiceManager.EnterChatSession();
    m_VoiceTimer.StartZero();

#ifdef SPEEX_LOOPBACK
    g_VoiceManager.AddChatter(g_speexLoopbackPlayerId);
#endif

  }
#endif
}

void CKaiClient::VoiceChatStop()
{
#ifdef HAS_KAI_VOICE
  if (m_pDSound)
  {
    CLog::Log(LOGINFO, "KAICLIENT: Releasing DirectSound...");
    g_VoiceManager.LeaveChatSession();
    // TODO: we really ought to shutdown, but somethings up - I can never seem to reinit properly.
    //  g_VoiceManager.Shutdown();
    m_pDSound->Release();
    m_pDSound = NULL;
    m_pEgress->Flush();
    CLog::Log(LOGINFO, "KAICLIENT: Voice chat disabled.");
  }
#endif
}

CKaiClient::~CKaiClient(void)
{
  VoiceChatStop();
  Destroy();

#ifdef HAS_KAI_VOICE
  if (m_pEgress)
    delete m_pEgress;
  m_pEgress=NULL;
#endif

  if (m_pRequestList)
    delete m_pRequestList;
  m_pRequestList=NULL;
}

CKaiClient* CKaiClient::GetInstance()
{
  if (client == NULL)
  {
    client = new CKaiClient();
  }

  return client;
}

void CKaiClient::RemoveInstance()
{
  if (client)
    delete client;
  client=NULL;
}

void CKaiClient::SetObserver(IBuddyObserver* aObserver)
{
  if (observer != aObserver)
  {
    m_dwSettlingTimer = timeGetTime();
    m_dwReachableTimer = timeGetTime();

    // Enable call back methods
    observer = aObserver;

    aObserver->OnInitialise(this);

    if (g_guiSettings.GetString("xlinkkai.server").size())
    {
      SOCKADDR_IN sockAddr;
      memset(&sockAddr, 0, sizeof(sockAddr));
      sockAddr.sin_family = AF_INET;
      sockAddr.sin_addr.s_addr = inet_addr(g_guiSettings.GetString("xlinkkai.server").c_str());
      sockAddr.sin_port = htons(KAI_SYSTEM_PORT);

      Attach(sockAddr);
    }
    else
    {
      Discover();
    }
  }
}
void CKaiClient::RemoveObserver()
{
  if (observer)
  {
    observer->OnDeInitialise();
    observer = NULL;
    Detach();
  }
}

void CKaiClient::EnterVector(CStdString& aVector, CStdString& aPassword)
{
  if (client_state == Authenticated)
  {
    CStdString strVectorMessage;
    strVectorMessage.Format("KAI_CLIENT_VECTOR;%s;%s;", aVector, aPassword);
    Send(server_addr, strVectorMessage);
    client_vector = aVector;
  }
}

void CKaiClient::JoinTextChat()
{
  if (client_state == Authenticated)
  {
    CStdString strVectorMessage;
    strVectorMessage.Format("KAI_CLIENT_CHATMODE;%s;", client_vector);
    Send(server_addr, strVectorMessage);
  }
}

void CKaiClient::Chat(CStdString& aMessage)
{
  if (client_state == Authenticated)
  {
    CStdString strChatMessage;
    strChatMessage.Format("KAI_CLIENT_CHAT;%s;", aMessage);
    Send(server_addr, strChatMessage);
  }
}

void CKaiClient::AddContact(CStdString& aContact)
{
  if (client_state == Authenticated)
  {
    CStdString strVectorMessage;
    strVectorMessage.Format("KAI_CLIENT_ADD_CONTACT;%s;", aContact);
    Send(server_addr, strVectorMessage);
  }
}

void CKaiClient::RemoveContact(CStdString& aContact)
{
  if (client_state == Authenticated)
  {
    CStdString strVectorMessage;
    strVectorMessage.Format("KAI_CLIENT_REMOVE_CONTACT;%s;", aContact);
    Send(server_addr, strVectorMessage);
  }
}

void CKaiClient::Invite(CStdString& aPlayer, CStdString& aVector, CStdString& aMessage)
{
  if (client_state == Authenticated)
  {
    CStdString strInvitationMessage;
    strInvitationMessage.Format("KAI_CLIENT_INVITE;%s;%s;%s;", aPlayer, aVector, aMessage);

    //OutputDebugString(strInvitationMessage.c_str());
    //OutputDebugString("\r\n");

    Send(server_addr, strInvitationMessage);
  }
}

void CKaiClient::Host()
{
  if (client_state == Authenticated)
  {
    SetHostingStatus(TRUE);
  }
}

void CKaiClient::Host(CStdString& aPassword, CStdString& aDescription, int aPlayerLimit)
{
  if (client_state == Authenticated)
  {
    CStdString strHostMessage;
    strHostMessage.Format("KAI_CLIENT_CREATE_VECTOR;%d;%s;%s;", aPlayerLimit, aDescription, aPassword);
    Send(server_addr, strHostMessage);
  }
}
void CKaiClient::SetHostingStatus(BOOL bIsHosting)
{
  if (client_state == Authenticated)
  {
    CStdString strStatusMessage;
    strStatusMessage.Format("KAI_CLIENT_ARENA_STATUS;%d;%d;", bIsHosting ? 2 : 1, 1);
    Send(server_addr, strStatusMessage);
  }
}

void CKaiClient::GetSubVectors(CStdString& aVector)
{
  if (client_state == Authenticated)
  {
    CStdString strGetSubArenasMessage;
    strGetSubArenasMessage.Format("KAI_CLIENT_GET_VECTORS;%s;", aVector);
    Send(server_addr, strGetSubArenasMessage);
  }
}

void CKaiClient::ExitVector()
{
  if (client_state == Authenticated)
  {
    INT vectorDelimiter = client_vector.ReverseFind('/');
    if (vectorDelimiter > 0)
    {
      CStdString previousVector = client_vector.Mid(0, vectorDelimiter);
      CStdString strPassword = "";
      EnterVector(previousVector, strPassword);
    }
  }
}

CStdString CKaiClient::GetCurrentVector()
{
  return client_vector;
}


void CKaiClient::Discover()
{
  CLog::Log(LOGNOTICE, "KAICLIENT: Discovering Kai engine via UDP broadcast." );
  client_state = Discovering;
  CStdString strInitiateDiscoveryMessage = "KAI_CLIENT_DISCOVER;";
  Broadcast(KAI_SYSTEM_PORT, strInitiateDiscoveryMessage);
}

void CKaiClient::Detach()
{
  if (client_state == Authenticated)
  {
    CStdString strDisconnectionMessage = "KAI_CLIENT_DETACH;";
    client_state = Disconnected;
    Send(server_addr, strDisconnectionMessage);
  }
}


void CKaiClient::Reattach()
{
  m_bHosting = FALSE;
  m_bContactsSettling = TRUE;
  m_nFriendsOnline = 0;

  if (g_guiSettings.GetString("xlinkkai.server").size())
  {
    SOCKADDR_IN sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));
    sockAddr.sin_family = AF_INET;
    sockAddr.sin_addr.s_addr = inet_addr(g_guiSettings.GetString("xlinkkai.server").c_str());
    sockAddr.sin_port = htons(KAI_SYSTEM_PORT);

    Attach(sockAddr);
  }
  else
  {
    Discover();
  }
}
void CKaiClient::Attach(SOCKADDR_IN& aAddress)
{
  char szIP[32];
  IN_ADDR host;
  memcpy(&host, &aAddress.sin_addr, sizeof(host));
#ifdef HAS_KAI_VOICE
  XNetInAddrToString(host, szIP, 32);
#endif

  CLog::Log(LOGNOTICE, "KAICLIENT: Attach to Kai engine host at %s", szIP);

  client_state = Attaching;
  CStdString strAttachMessage = "KAI_CLIENT_ATTACH;";
  server_addr = aAddress;
  Send(server_addr, strAttachMessage);
}

void CKaiClient::TakeOver()
{
  client_state = Attaching;
  CStdString strTakeOverMessage = "KAI_CLIENT_TAKEOVER;";
  Send(server_addr, strTakeOverMessage);
}

void CKaiClient::Query()
{
  client_state = Querying;
  CStdString strQueryMessage = "KAI_CLIENT_GETSTATE;";
  Send(server_addr, strQueryMessage);
}

void CKaiClient::Login(LPCSTR aUsername, LPCSTR aPassword)
{
  client_state = LoggingIn;
  CStdString strLoginMessage;
  strLoginMessage.Format("KAI_CLIENT_LOGIN;%s;%s;", aUsername, aPassword);
  Send(server_addr, strLoginMessage);
}
bool CKaiClient::IsEngineConnected()
{
  return client_state == Authenticated;
}
bool CKaiClient::IsNetworkReachable()
{
  return (m_bReachable && client_state == Authenticated);
}
void CKaiClient::QueryUserProfile(CStdString& aPlayerName)
{
  if (client_state == Authenticated)
  {
    CStdString strQueryMessage;
    strQueryMessage.Format("KAI_CLIENT_GET_PROFILE;%s;", aPlayerName.c_str());
    Send(server_addr, strQueryMessage);
  }
}

void CKaiClient::QueryAvatar(CStdString& aPlayerName)
{
  if (client_state == Authenticated)
  {
    CStdString strQueryMessage;
    strQueryMessage.Format("KAI_CLIENT_AVATAR;%s;", aPlayerName.c_str());
    Send(server_addr, strQueryMessage);
  }
}
void CKaiClient::QueryClientMetrics()
{
  if (client_state == Authenticated)
  {
    CStdString strQueryMessage = "KAI_CLIENT_GET_METRICS;";
    Send(server_addr, strQueryMessage);
  }
}
void CKaiClient::SetBearerCaps(BOOL bIsHeadsetPresent)
{
  if (client_state == Authenticated)
  {
    CStdString strStatusMessage;
    strStatusMessage.Format("KAI_CLIENT_CAPS;01%s3;", bIsHeadsetPresent ? "2" : "" );
    Send(server_addr, strStatusMessage);
  }
}

void CKaiClient::EnableContactVoice(CStdString& aContactName, BOOL bEnable)
{
  if (client_state == Authenticated)
  {
    CStdString strVoiceMessage;
    strVoiceMessage.Format("KAI_CLIENT_SPEEX_%s;%s;", bEnable ? "ON" : "OFF",
                           aContactName.c_str());
    Send(server_addr, strVoiceMessage);
  }
}

void CKaiClient::QueueContactVoice(CStdString& aContactName, DWORD aPlayerId, LPBYTE pMessage, DWORD dwMessageLength)
{
#ifdef HAS_KAI_VOICE
  if (client_state == Authenticated)
  {
    CStdString header;
    header.Format("KAI_CLIENT_SPEEX;%s;", aContactName.c_str());

    int frames = 0;
    for (DWORD messageOffset = header.length(); messageOffset < dwMessageLength; messageOffset += COMPRESSED_FRAME_SIZE)
    {
      frames++;

      // we need to persist this buffer before we can have it queued
      g_VoiceManager.ReceivePacket(
        aPlayerId,
        (LPVOID) &pMessage[messageOffset],
        COMPRESSED_FRAME_SIZE );
    }

    //char szDebug[128];
    //sprintf(szDebug,"RX KAI SPEEX %d frames, total: %u bytes.\r\n",frames,dwMessageLength);
    //OutputDebugString(szDebug);
  }
#endif
}


void CKaiClient::SendVoiceDataToEngine()
{
#ifdef HAS_KAI_VOICE
  if (client_state == Authenticated)
  {
    if (!m_pEgress->IsEmpty())
    {
      CHAR* buffer = new CHAR[700];

#ifndef SPEEX_LOOPBACK
      sprintf(buffer, "KAI_CLIENT_SPEEX;");
#else
      sprintf(buffer, "KAI_CLIENT_SPEEX;SpeexLoopback;");
#endif

      int nPackets = m_pEgress->Size();

      //char szDebug[128];
      //sprintf(szDebug,"Sending %d packets\r\n", nPackets);
      //OutputDebugString(szDebug);

      int messageOffset = strlen(buffer);
      for (int i = 0; i < nPackets; i++, messageOffset += COMPRESSED_FRAME_SIZE)
      {
        XMEDIAPACKET outboundPacket;
        m_pEgress->Read(outboundPacket);

        memcpy((LPVOID)&buffer[messageOffset], outboundPacket.pvBuffer, COMPRESSED_FRAME_SIZE);

        *outboundPacket.pdwStatus = XMEDIAPACKET_STATUS_SUCCESS;
      }

#ifndef SPEEX_LOOPBACK
      Send(server_addr, (LPBYTE)buffer, messageOffset);
#else
      CStdString strName = "SpeexLoopback";
      QueueContactVoice(strName, g_speexLoopbackPlayerId, (LPBYTE)buffer, messageOffset);
#endif

    }
  }

#endif
  // reset buffer and stopwatch
  m_VoiceTimer.StartZero();
}




void CKaiClient::OnMessage(SOCKADDR_IN& aRemoteAddress, CStdString& aMessage, LPBYTE pMessage, DWORD dwMessageLength)
{
  CHAR* szMessage = strtok( (char*)((LPCSTR)aMessage), ";");

  // now depending on state...
  switch (client_state)
  {
  case Discovering:
    if (strcmp(szMessage, "KAI_CLIENT_ENGINE_HERE") == 0)
    {
      // if no server setting is stored, store engine address provided in response to discover message.
      if (g_guiSettings.GetString("xlinkkai.server").IsEmpty())
      {
        IN_ADDR server;
        memcpy(&server, &aRemoteAddress.sin_addr, sizeof(server));
        char strKaiServer[256];
        strcpy(strKaiServer, g_guiSettings.GetString("xlinkkai.server").c_str());
#ifdef HAS_KAI_VOICE
        XNetInAddrToString(server, strKaiServer, 32);
#endif
        g_guiSettings.SetString("xlinkkai.server", strKaiServer);
      }

      Attach(aRemoteAddress);
    }
    break;

  case Attaching:
    if (strcmp(szMessage, "KAI_CLIENT_ATTACH") == 0)
    {
      Query();
    }
    else if (strcmp(szMessage, "KAI_CLIENT_ENGINE_IN_USE") == 0)
    {
      TakeOver();
    }
    break;

  case Querying:
    if (strcmp(szMessage, "KAI_CLIENT_LOGGED_IN") == 0)
    {
      client_state = Authenticated;
      m_dwSettlingTimer = timeGetTime();
      m_dwReachableTimer = timeGetTime();
    }
    else if (strcmp(szMessage, "KAI_CLIENT_NOT_LOGGED_IN") == 0)
    {
      // has XBMC user set up KAI username and password?
      if (g_guiSettings.GetString("xlinkkai.username").IsEmpty())
      {
        CStdString strDefaultUsername = strtok(NULL, ";");
        CStdString strDefaultPassword = strtok(NULL, ";");

        // has KAI engine provided us with a default username and password?
        if (strDefaultUsername.length() > 0 && strDefaultPassword.length() > 0)
        {
          g_guiSettings.SetString("xlinkkai.username", strDefaultUsername.c_str());
          g_guiSettings.SetString("xlinkkai.password", strDefaultPassword.c_str());

          // auto login with KAI credentials
          Login(g_guiSettings.GetString("xlinkkai.username"), g_guiSettings.GetString("xlinkkai.password"));
        }
      }
      else
      {
        // auto login with XBMC credentials
        Login(g_guiSettings.GetString("xlinkkai.username"), g_guiSettings.GetString("xlinkkai.password"));
      }
    }
    break;

  case LoggingIn:
    if (strcmp(szMessage, "KAI_CLIENT_USER_DATA") == 0)
    {
      client_state = Authenticated;
      m_dwSettlingTimer = timeGetTime();
      m_dwReachableTimer = timeGetTime();

      CStdString strCaseCorrectedName = strtok(NULL, ";");
      if (strCaseCorrectedName.length() > 0)
      {
        g_guiSettings.SetString("xlinkkai.username", strCaseCorrectedName.c_str());
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_AUTHENTICATION_FAILED") == 0)
    {
      if (observer != NULL)
      {
        CStdString strUsername = g_guiSettings.GetString("xlinkkai.username");
        observer->OnAuthenticationFailed(strUsername);
      }
    }
    break;

  case Authenticated:

    if ((strcmp(szMessage, "KAI_CLIENT_ADD_CONTACT") == 0) ||
        (strcmp(szMessage, "KAI_CLIENT_CONTACT_OFFLINE") == 0))

    {
      m_nFriendsOnline--;
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");

        observer->OnContactOffline( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_CONTACT_ONLINE") == 0)
    {
      m_nFriendsOnline++;
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");

        observer->OnContactOnline( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX_ON") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");
        observer->OnContactSpeexStatus(strContactName, true);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX_OFF") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");
        observer->OnContactSpeexStatus(strContactName, false);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX_START") == 0)
    {
      VoiceChatStart();
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX") == 0)
    {
      CStdString strContactName = strtok(NULL, ";");

#ifdef HAS_KAI_VOICE
      if (m_pDSound)
      {
        DWORD playerId = Crc32FromString(strContactName);
        BOOL bRegistered = g_VoiceManager.IsPlayerRegistered( playerId );
        BOOL bAvailable = TRUE;

        if (!bRegistered)
        {
          //CStdString strDebug;
          //strDebug.Format("Adding chatter %s\r\n",strContactName.c_str());
          //OutputDebugString(strDebug.c_str());
          bAvailable = SUCCEEDED(g_VoiceManager.AddChatter(playerId));
        }
        if (bAvailable)
        {
          QueueContactVoice(strContactName, playerId, pMessage, dwMessageLength);
          if (observer != NULL)
          {
            observer->OnContactSpeex(strContactName);
          }
        }
        else
        {
          CLog::Log(LOGERROR, "Failed to queue contact voice.");
        }
      }
#endif
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX_STOP") == 0)
    {
      VoiceChatStop();
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPEEX_RING") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");
        observer->OnContactSpeexRing( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_CONTACT_PING") == 0)
    {
      if (observer != NULL)
      {
        CHAR* szContact = strtok(NULL, ";");
        CStdString strContactName = szContact;
        CStdString strVector = "Home";

        if ( szContact[strlen(szContact) + 1] != ';' )
        {
          strVector = strtok(NULL, ";");
        }

        CStdString strPing = strtok(NULL, ";");
        CStdString strCaps = strtok(NULL, ";");

        observer->OnContactPing(strContactName, strVector,
                                strtoul(strPing.c_str(), NULL, 10), 0, strCaps);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_REMOVE_CONTACT") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");

        observer->OnContactRemove( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_INVITE") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");
        CStdString strVector = strtok(NULL, ";");
        CStdString strTime = strtok(NULL, ";");
        CStdString strMessage = strtok(NULL, ";");

        observer->OnContactInvite( strContactName, strVector, strTime, strMessage );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_VECTOR") == 0)
    {
      CStdString strVector = strtok(NULL, ";");

      bool bInPrivateRoom = strVector.Find(g_guiSettings.GetString("xlinkkai.username")) > 0;

      // check to see if we've left our private room and we're still hosting
      if (m_bHosting)
      {
        if (!bInPrivateRoom)
        {
          // set our status to looking to join games as opposed to hosting them
          SetHostingStatus(FALSE);
        }
      }
      else
      {
        if (bInPrivateRoom)
        {
          // set our status to looking to host games as opposed to joining them
          SetHostingStatus(TRUE);
        }
      }

      CStdString strCanCreate = strtok(NULL, ";");
      if (observer != NULL)
      {
        client_vector = strVector;
        observer->OnEnterArena(strVector, atoi(strCanCreate.c_str()));
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SUB_VECTOR") == 0)
    {
      CStdString strVector = strtok(NULL, ";");
      CStdString strPlayers = strtok(NULL, ";");
      CStdString strSubVectors = strtok(NULL, ";");
      CStdString strIsPrivate = strtok(NULL, ";");
      CStdString strPlayerLimit = strtok(NULL, ";");
      CStdString strDescription = "Public Arena";

      if (observer != NULL)
      {
        observer->OnNewArena( strVector,
                              strDescription,
                              atoi(strPlayers.c_str()),
                              atoi(strPlayerLimit.c_str()),
                              atoi(strIsPrivate.c_str()),
                              false );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_ARENA_STATUS") == 0)
    {
      CStdString strMode = strtok(NULL, ";");
      m_bHosting = atoi(strMode.c_str()) > 1;

      if (observer != NULL)
      {
        observer->OnUpdateHostingStatus(m_bHosting);
      }
      CLog::Log(LOGINFO, m_bHosting ? "KAICLIENT: User intends to host games." : "KAICLIENT: User intends to join games.");
    }

    else if (strcmp(szMessage, "KAI_CLIENT_USER_SUB_VECTOR") == 0)
    {
      CStdString strVector = strtok(NULL, ";");
      CStdString strPlayers = strtok(NULL, ";");
      CStdString strSubVectors = strtok(NULL, ";");
      CStdString strIsPrivate = strtok(NULL, ";");
      CStdString strPlayerLimit = strtok(NULL, ";");
      CStdString strDescription = strtok(NULL, ";");

      if (observer != NULL)
      {
        observer->OnNewArena( strVector,
                              strDescription,
                              atoi(strPlayers.c_str()),
                              atoi(strPlayerLimit.c_str()),
                              atoi(strIsPrivate.c_str()),
                              true );
      }
    }

    else if (strcmp(szMessage, "KAI_CLIENT_SUB_VECTOR_UPDATE") == 0)
    {
      CStdString strVector = strtok(NULL, ";");
      CStdString strPlayers = strtok(NULL, ";");

      if (observer != NULL)
      {
        observer->OnUpdateArena( strVector,
                                 atoi(strPlayers.c_str()) );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_VECTOR_DISALLOWED") == 0)
    {
      CStdString strVector = strtok(NULL, ";");
      CStdString strReason = strtok(NULL, ";");

      if (observer != NULL)
      {
        observer->OnEnterArenaFailed( strVector, strReason );
      }
    }

    else if (strcmp(szMessage, "KAI_CLIENT_JOINS_VECTOR") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");

        observer->OnOpponentEnter( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_ARENA_PING") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");
        CStdString strPing = strtok(NULL, ";");
        CStdString strStatus = strtok(NULL, ";"); // 0 - idle, 1 - join, 2- host, 3-dedicated host
        CStdString strPlayers = strtok(NULL, ";");
        CStdString strCaps = strtok(NULL, ";");
        observer->OnOpponentPing(strContactName,
                                 strtoul(strPing.c_str(), NULL, 10), atoi(strStatus.c_str()), strCaps);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_USER_PROFILE") == 0)
    {
      if (observer != NULL)
      {
        CStdString strOpponent = strtok(NULL, ";");
        CStdString strAge = strtok(NULL, ";");
        CStdString strBandwidth = strtok(NULL, ";");
        CStdString strLocation = strtok(NULL, ";");
        CStdString strXBOX = strtok(NULL, ";");
        CStdString strGCN = strtok(NULL, ";");
        CStdString strPS2 = strtok(NULL, ";");
        CStdString strBio = strtok(NULL, ";");

        observer->OnUpdateOpponent(strOpponent, strAge, strBandwidth, strLocation, strBio );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_AVATAR") == 0)
    {
      if (observer != NULL)
      {
        CStdString strOpponent = strtok(NULL, ";");
        CStdString strUrl = strtok(NULL, ";");

        if (strUrl.length() > 0)
        {
          observer->OnUpdateOpponent(strOpponent, strUrl);
        }
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_LEAVES_VECTOR") == 0)
    {
      if (observer != NULL)
      {
        CStdString strContactName = strtok(NULL, ";");

        observer->OnOpponentLeave( strContactName );
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_APP_SPECIFIC") == 0)
    {
      if (observer != NULL)
      {
        CStdString strQuestion = strtok(NULL, ";");
        CStdString strAnswer = strtok(NULL, ";");

        if (strAnswer.length() > 0 && strQuestion.Find("XBE_") == 0)
        {
          CStdString strTitleId = strQuestion.Mid(4);
          DWORD dwTitleId = strtoul(strTitleId.c_str(), NULL, 16);
          observer->OnSupportedTitle(dwTitleId, strAnswer );
          QueryVectorPlayerCount(strAnswer);
        }
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_SPECIFIC_COUNT") == 0)
    {
      if (observer != NULL)
      {
        CStdString strVector = strtok(NULL, ";");
        CStdString strPlayers = strtok(NULL, ";");
        observer->OnUpdateArena(strVector, atoi(strPlayers));
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_JOINS_CHAT") == 0)
    {
      if (observer != NULL)
      {
        CStdString strVector = strtok(NULL, ";");
        CStdString strOpponent = strtok(NULL, ";");

        if (strVector.CompareNoCase(client_vector) == 0)
        {
          observer->OnJoinsChat(strOpponent);
        }
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_CHAT") == 0)
    {
      if (observer != NULL)
      {
        CStdString strVector = strtok(NULL, ";");
        CStdString strOpponent = strtok(NULL, ";");
        CStdString strMessage = strtok(NULL, ";");
        observer->OnChat(strVector, strOpponent, strMessage, false);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_ARENA_PM") == 0)
    {
      if (observer != NULL)
      {
        CStdString strOpponent = strtok(NULL, ";");
        CStdString strMessage = strtok(NULL, ";");
        observer->OnChat(client_vector, strOpponent, strMessage, true);
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_LEAVES_CHAT") == 0)
    {
      if (observer != NULL)
      {
        CStdString strVector = strtok(NULL, ";");
        CStdString strOpponent = strtok(NULL, ";");

        if (strVector.CompareNoCase(client_vector) == 0)
        {
          observer->OnLeavesChat(strOpponent);
        }
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_METRICS") == 0)
    {
      if (observer != NULL)
      {
        CStdString strServer = strtok(NULL, ";");
        CStdString strReachable = strtok(NULL, ";");

        if (strReachable.Find("Yes") >= 0)
        {
          m_bReachable = TRUE;
          int nameIsLong = strServer.Find('(') - 1;
          CStdString strName = nameIsLong > 0 ? strServer.Left(nameIsLong) : strServer;
          observer->OnNetworkReachable(strName);
        }
      }
    }
    else if (strcmp(szMessage, "KAI_CLIENT_DETACH") == 0)
    {
      client_state = Disconnected;
      if (observer != NULL)
      {
        observer->OnEngineDetached();
      }
    }

    break;
  }

  // do infrequent work
  if (client_state == Authenticated)
  {
    if (!m_bContactsSettling)
    {
#ifdef HAS_KAI_VOICE
      BOOL bHeadset = CVoiceManager::IsHeadsetConnected();
      if (bHeadset != m_bHeadset)
      {
        SetBearerCaps(bHeadset);
        m_bHeadset = bHeadset;
      }
      else
      {
        CStdString strRequest = "";
        if (m_pRequestList->GetNext(strRequest))
        {
          Send(server_addr, strRequest);
        }
      }
#endif
    }
  }
}

// do frequent work
void CKaiClient::DoWork()
{
  // generate our own timed events
  if (observer != NULL && client_state == Authenticated)
  {
    DWORD dwCurrentTime = timeGetTime();

    if (m_bContactsSettling && (dwCurrentTime - m_dwSettlingTimer > KAI_CONTACT_SETTLING_PERIOD) )
    {
      m_bContactsSettling = FALSE;
      observer->OnContactsOnline(m_nFriendsOnline);

#ifdef HAS_KAI_VOICE
      m_bHeadset = CVoiceManager::IsHeadsetConnected();
      SetBearerCaps(m_bHeadset);
#endif
    }

    if (!m_bReachable && (dwCurrentTime - m_dwReachableTimer > KAI_REACHABLE_QUERY_PERIOD) )
    {
      m_dwReachableTimer = dwCurrentTime;
      QueryClientMetrics();
    }
  }

#ifdef HAS_KAI_VOICE
  if (m_pDSound)
  {
    g_VoiceManager.ProcessVoice();

    // Make sure we send voice data at an appropriate rate
    if ( m_VoiceTimer.GetElapsedSeconds() > VOICE_SEND_INTERVAL )
    {
      SendVoiceDataToEngine();
    }
  }
#endif
}
#ifdef HAS_KAI_VOICE
// Called whenever voice data is produced by the voice system
void CKaiClient::VoiceDataCallback( DWORD dwPort, DWORD dwSize, VOID* pvData, VOID* pContext )
{
  CKaiClient* pThis = (CKaiClient*)pContext;
  pThis->OnVoiceData( dwPort, dwSize, pvData );
}
#endif

void CKaiClient::OnVoiceData( DWORD dwControllerPort, DWORD dwSize, VOID* pvData )
{
#ifdef HAS_KAI_VOICE
  if (m_pDSound)
  {
    m_pEgress->Write((LPBYTE)pvData);

    // We've set up our voice timer such that it SHOULD cause us to send out
    // our buffered voice data before the buffer fills up.  However, things
    // like framerate glitches, etc., could cause us to fill up before we
    // notice the timer has fired.
    if ( m_pEgress->Size() == MAX_VOICE_PER_SEND )
    {
      SendVoiceDataToEngine();
    }
  }
#endif
}



#ifdef HAS_KAI_VOICE
// Called whenever a voice communicator event occurs e.g. insertions/removals
void CKaiClient::CommunicatorCallback( DWORD dwPort, VOICE_COMMUNICATOR_EVENT event, VOID* pContext )
{
  CKaiClient* pThis = (CKaiClient*)pContext;
  pThis->OnCommunicatorEvent( dwPort, event );
}

void CKaiClient::OnCommunicatorEvent( DWORD dwControllerPort, VOICE_COMMUNICATOR_EVENT event )
{
  switch ( event )
  {
  case VOICE_COMMUNICATOR_INSERTED:
    CLog::Log(LOGINFO, "Voice communicator inserted.");
    g_VoiceManager.SetLoopback( dwControllerPort, FALSE );
    SetBearerCaps(TRUE);
    break;

  case VOICE_COMMUNICATOR_REMOVED:
    CLog::Log(LOGINFO, "Voice communicator removed.");
    SetBearerCaps(FALSE);
    break;
  }
}
#endif

DWORD CKaiClient::Crc32FromString(CStdString& aString)
{
  Crc32 crc;
  crc.Compute(aString);
  return (DWORD)crc;
}
