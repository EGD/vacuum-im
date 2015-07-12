#include "multiuserchat.h"

#include <definitions/namespaces.h>
#include <definitions/dataformtypes.h>
#include <definitions/multiuserdataroles.h>
#include <definitions/messageeditororders.h>
#include <definitions/stanzahandlerorders.h>
#include <utils/pluginhelper.h>
#include <utils/xmpperror.h>
#include <utils/logger.h>

#define SHC_PRESENCE        "/presence"
#define SHC_MESSAGE         "/message"

#define MUC_IQ_TIMEOUT      30000
#define MUC_LIST_TIMEOUT    60000

MultiUserChat::MultiUserChat(const Jid &AStreamJid, const Jid &ARoomJid, const QString &ANickName, const QString &APassword, bool AIsolated, QObject *AParent) : QObject(AParent)
{
	FMainUser = NULL;
	FSHIMessage = -1;
	FSHIPresence = -1;
	FConnected = false;
	FAutoPresence = false;

	FIsolated = AIsolated;
	FRoomJid = ARoomJid;
	FStreamJid = AStreamJid;
	FNickName = ANickName;
	FPassword = APassword;

	FStanzaProcessor = PluginHelper::pluginInstance<IStanzaProcessor>();
	if (FStanzaProcessor)
	{
		IStanzaHandle shandle;
		shandle.handler = this;
		shandle.order = SHO_PI_MULTIUSERCHAT;
		shandle.direction = IStanzaHandle::DirectionIn;
		shandle.streamJid = FStreamJid;
		shandle.conditions.append(SHC_PRESENCE);
		FSHIPresence = FStanzaProcessor->insertStanzaHandle(shandle);
	}

	FMessageProcessor = PluginHelper::pluginInstance<IMessageProcessor>();
	if (FIsolated && FStanzaProcessor)
	{
		IStanzaHandle shandle;
		shandle.handler = this;
		shandle.order = SHO_MI_MULTIUSERCHAT;
		shandle.direction = IStanzaHandle::DirectionIn;
		shandle.streamJid = FStreamJid;
		shandle.conditions.append(SHC_MESSAGE);
		FSHIMessage = FStanzaProcessor->insertStanzaHandle(shandle);
	}
	else if (!FIsolated && FMessageProcessor)
	{
		FMessageProcessor->insertMessageEditor(MEO_MULTIUSERCHAT,this);
	}

	FPresenceManager = PluginHelper::pluginInstance<IPresenceManager>();
	if (FPresenceManager)
	{
		connect(FPresenceManager->instance(),SIGNAL(presenceChanged(IPresence *, int, const QString &, int)),SLOT(onPresenceChanged(IPresence *, int, const QString &, int)));
	}

	IXmppStreamManager *xmppStreamManager = PluginHelper::pluginInstance<IXmppStreamManager>();
	if (xmppStreamManager)
	{
		connect(xmppStreamManager->instance(),SIGNAL(streamClosed(IXmppStream *)),SLOT(onXmppStreamClosed(IXmppStream *)));
		connect(xmppStreamManager->instance(),SIGNAL(streamJidChanged(IXmppStream *,const Jid &)),SLOT(onXmppStreamJidChanged(IXmppStream *,const Jid &)));
	}

	FDiscovery = PluginHelper::pluginInstance<IServiceDiscovery>();
	if (FDiscovery)
	{
		connect(FDiscovery->instance(),SIGNAL(discoInfoReceived(const IDiscoInfo &)),SLOT(onDiscoveryInfoReceived(const IDiscoInfo &)));
	}

	FDataForms = PluginHelper::pluginInstance<IDataForms>();
}

MultiUserChat::~MultiUserChat()
{
	if (!FUsers.isEmpty())
		closeChat(IPresenceItem());

	if (FStanzaProcessor)
	{
		FStanzaProcessor->removeStanzaHandle(FSHIMessage);
		FStanzaProcessor->removeStanzaHandle(FSHIPresence);
	}
	if (FMessageProcessor)
	{
		FMessageProcessor->removeMessageEditor(MEO_MULTIUSERCHAT,this);
	}
	emit chatDestroyed();
}

bool MultiUserChat::stanzaReadWrite(int AHandlerId, const Jid &AStreamJid, Stanza &AStanza, bool &AAccept)
{
	if (AStreamJid==FStreamJid && FRoomJid.pBare()==Jid(AStanza.from()).pBare())
	{
		AAccept = true;
		if (AHandlerId == FSHIPresence)
			processPresence(AStanza);
		else if (AHandlerId == FSHIMessage)
			processMessage(AStanza);
		return true;
	}
	return false;
}

void MultiUserChat::stanzaRequestResult(const Jid &AStreamJid, const Stanza &AStanza)
{
	if (FRoleUpdateId.contains(AStanza.id()))
	{
		QString nick = FRoleUpdateId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("User role updated, room=%1, user=%2, id=%3").arg(FRoomJid.bare(),nick,AStanza.id()));
			emit userRoleUpdated(AStanza.id(), nick);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to update user role, room=%1, user=%2, id=%3: %4").arg(FRoomJid.bare(),nick,AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(), err);
		}
	}
	else if (FAffilUpdateId.contains(AStanza.id()))
	{
		QString nick = FAffilUpdateId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("User affiliation updated, room=%1, user=%2, id=%3").arg(FRoomJid.bare(),nick,AStanza.id()));
			emit userAffiliationUpdated(AStanza.id(),nick);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to update user affiliation, room=%1, user=%2, id=%3: %4").arg(FRoomJid.bare(),nick,AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(), err);
		}
	}
	else if (FAffilListLoadId.contains(AStanza.id()))
	{
		QString affiliation = FAffilListLoadId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("Conference affiliation list loaded, room=%1, affiliation=%2, id=%3").arg(FRoomJid.bare(),affiliation,AStanza.id()));

			QList<IMultiUserListItem> items;
			QDomElement itemElem = AStanza.firstElement("query",NS_MUC_ADMIN).firstChildElement("item");
			while (!itemElem.isNull())
			{
				if (itemElem.attribute("affiliation") == affiliation)
				{
					IMultiUserListItem item;
					item.realJid = itemElem.attribute("jid");
					item.affiliation = itemElem.attribute("affiliation");
					item.reason = itemElem.firstChildElement("reason").text();
					items.append(item);
				}
				itemElem = itemElem.nextSiblingElement("item");
			}
			emit affiliationListLoaded(AStanza.id(), items);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to load conference affiliation list, room=%1, affiliation=%2, id=%3: %4").arg(FRoomJid.bare(),affiliation,AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(), err);
		}
	}
	else if (FAffilListUpdateId.contains(AStanza.id()))
	{
		QList<IMultiUserListItem> items = FAffilListUpdateId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("Conference affiliation list updated, room=%1, id=%2").arg(FRoomJid.bare(),AStanza.id()));
			emit affiliationListUpdated(AStanza.id(), items);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to update conference affiliation list, room=%1, id=%2: %3").arg(FRoomJid.bare(),AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(), err);
		}
	}
	else if (FConfigLoadId.contains(AStanza.id()))
	{
		FConfigLoadId.removeAll(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("Conference configuration loaded, room=%1, id=%2").arg(FRoomJid.bare(), AStanza.id()));
			QDomElement formElem = AStanza.firstElement("query",NS_MUC_OWNER).firstChildElement("x");
			while (formElem.namespaceURI() != NS_JABBER_DATA)
				formElem = formElem.nextSiblingElement("x");
			emit roomConfigLoaded(AStanza.id(),FDataForms->dataForm(formElem));
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to load conference configuration, room=%1, id=%2: %3").arg(FRoomJid.bare(),AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(),err);
		}
	}
	else if (FConfigUpdateId.contains(AStanza.id()))
	{
		IDataForm form = FConfigUpdateId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("Conference configuration updated, room=%1, id=%2").arg(FRoomJid.bare(),AStanza.id()));
			emit roomConfigUpdated(AStanza.id(), form);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to update conference configuration, room=%1, id=%2: %3").arg(FRoomJid.bare(),AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(),err);
		}
	}
	else if (FRoomDestroyId.contains(AStanza.id()))
	{
		QString reason = FRoomDestroyId.take(AStanza.id());
		if (AStanza.type() == "result")
		{
			LOG_STRM_INFO(AStreamJid,QString("Conference destroyed, room=%1, id=%2").arg(FRoomJid.bare(),AStanza.id()));
			emit roomDestroyed(AStanza.id(),reason);
		}
		else
		{
			XmppStanzaError err(AStanza);
			LOG_STRM_WARNING(AStreamJid,QString("Failed to destroy conference, room=%1, id=%2: %3").arg(FRoomJid.bare(),AStanza.id(),err.condition()));
			emit requestFailed(AStanza.id(),err);
		}
	}
}

bool MultiUserChat::messageReadWrite(int AOrder, const Jid &AStreamJid, Message &AMessage, int ADirection)
{
	if (AOrder==MEO_MULTIUSERCHAT && ADirection==IMessageProcessor::DirectionIn && AStreamJid==FStreamJid)
	{
		if (FRoomJid.pBare() == Jid(AMessage.from()).pBare())
			return processMessage(AMessage.stanza());
	}
	return false;
}

bool MultiUserChat::isIsolated() const
{
	return FIsolated;
}

Jid MultiUserChat::streamJid() const
{
	return FStreamJid;
}

Jid MultiUserChat::roomJid() const
{
	return FRoomJid;
}

QString MultiUserChat::roomName() const
{
	return !FRoomName.isEmpty() ? FRoomName : roomShortName();
}

QString MultiUserChat::roomShortName() const
{
	return FRoomJid.uNode();
}

bool MultiUserChat::isOpen() const
{
	return FMainUser!=NULL;
}

bool MultiUserChat::isConnected() const
{
	return FConnected;
}

bool MultiUserChat::autoPresence() const
{
	return FAutoPresence;
}

void MultiUserChat::setAutoPresence(bool AAuto)
{
	FAutoPresence = AAuto;
}

QList<int> MultiUserChat::statusCodes() const
{
	return FStatusCodes;
}

QList<int> MultiUserChat::statusCodes(const Stanza &AStanza) const
{
	QList<int> codes;
	QDomElement statusElem =  AStanza.firstElement("x",NS_MUC_USER).firstChildElement("status");
	while (!statusElem.isNull())
	{
		codes.append(statusElem.attribute("code").toInt());
		statusElem = statusElem.nextSiblingElement("status");
	}
	return codes;
}

XmppError MultiUserChat::roomError() const
{
	return FRoomError;
}

IPresenceItem MultiUserChat::roomPresence() const
{
	return FRoomPresence;
}

IMultiUser *MultiUserChat::mainUser() const
{
	return FMainUser;
}

QList<IMultiUser *> MultiUserChat::allUsers() const
{
	QList<IMultiUser *> users;
	users.reserve(FUsers.count());

	foreach(MultiUser *user, FUsers)
		users.append(user);

	return users;
}

IMultiUser *MultiUserChat::findUser(const QString &ANick) const
{
	return FUsers.value(ANick);
}

bool MultiUserChat::isUserPresent(const Jid &AContactJid) const
{
	if (AContactJid.pBare() != FRoomJid.pBare())
	{
		foreach(MultiUser *user, FUsers)
			if (AContactJid == user->realJid())
				return true;
		return false;
	}
	return FUsers.contains(AContactJid.resource());
}

QString MultiUserChat::nickName() const
{
	return FNickName;
}

bool MultiUserChat::setNickName(const QString &ANick)
{
	if (!ANick.isEmpty())
	{
		if (isConnected() && FNickName!=ANick)
		{
			Jid userJid(FRoomJid.node(),FRoomJid.domain(),ANick);

			Stanza presence("presence");
			presence.setTo(userJid.full());
			if (FStanzaProcessor->sendStanzaOut(FStreamJid,presence))
			{
				LOG_STRM_INFO(FStreamJid,QString("Change conference nick request sent, room=%1, old=%2, new=%3").arg(FRoomJid.bare(),FNickName,ANick));
				return true;
			}
			else
			{
				LOG_STRM_WARNING(FStreamJid,QString("Failed to send change conference nick request, room=%1").arg(FRoomJid.bare()));
			}
		}
		else
		{
			FNickName = ANick;
			return true;
		}
	}
	else
	{
		REPORT_ERROR("Failed to change conference nick: Nick is empty");
	}
	return false;
}

QString MultiUserChat::password() const
{
	return FPassword;
}

void MultiUserChat::setPassword(const QString &APassword)
{
	FPassword = APassword;
}

IMultiUserChatHistory MultiUserChat::historyScope() const
{
	return FHistory;
}

void MultiUserChat::setHistoryScope(const IMultiUserChatHistory &AHistory)
{
	FHistory = AHistory;
}

bool MultiUserChat::sendStreamPresence()
{
	IPresence *presence = FPresenceManager!=NULL ? FPresenceManager->findPresence(FStreamJid) : NULL;
	return presence!=NULL ? sendPresence(presence->show(),presence->status(),presence->priority()) : false;
}

bool MultiUserChat::sendPresence(int AShow, const QString &AStatus, int APriority)
{
	if (FStanzaProcessor)
	{
		Jid userJid(FRoomJid.node(),FRoomJid.domain(),FNickName);

		Stanza presence("presence");
		presence.setTo(userJid.full());

		QString showText;
		bool isConnecting = true;
		switch (AShow)
		{
		case IPresence::Online:
			showText = QString::null;
			break;
		case IPresence::Chat:
			showText = "chat";
			break;
		case IPresence::Away:
			showText = "away";
			break;
		case IPresence::DoNotDisturb:
			showText = "dnd";
			break;
		case IPresence::ExtendedAway:
			showText = "xa";
			break;
		default:
			isConnecting = false;
			showText = "unavailable";
		}

		if (!isConnecting)
		{
			presence.setType("unavailable");
		}
		else
		{
			if (!showText.isEmpty())
				presence.addElement("show").appendChild(presence.createTextNode(showText));
			presence.addElement("priority").appendChild(presence.createTextNode(QString::number(APriority)));
		}

		if (!AStatus.isEmpty())
			presence.addElement("status").appendChild(presence.createTextNode(AStatus));

		if (!isConnected() && isConnecting)
		{
			FRoomError = XmppError::null;
			emit chatAboutToConnect();

			QDomElement xelem = presence.addElement("x",NS_MUC);
			if (FHistory.empty || FHistory.maxChars>0 || FHistory.maxStanzas>0 || FHistory.seconds>0 || FHistory.since.isValid())
			{
				QDomElement histElem = xelem.appendChild(presence.createElement("history")).toElement();
				if (!FHistory.empty)
				{
					if (FHistory.maxChars > 0)
						histElem.setAttribute("maxchars",FHistory.maxChars);
					if (FHistory.maxStanzas > 0)
						histElem.setAttribute("maxstanzas",FHistory.maxStanzas);
					if (FHistory.seconds > 0)
						histElem.setAttribute("seconds",FHistory.seconds);
					if (FHistory.since.isValid())
						histElem.setAttribute("since",DateTime(FHistory.since).toX85UTC());
				}
				else
				{
					histElem.setAttribute("maxchars",0);
				}
			}

			if (!FPassword.isEmpty())
				xelem.appendChild(presence.createElement("password")).appendChild(presence.createTextNode(FPassword));

			if (FDiscovery)
			{
				if (!FDiscovery->hasDiscoInfo(streamJid(),roomJid()))
					FDiscovery->requestDiscoInfo(streamJid(),roomJid());
				else
					onDiscoveryInfoReceived(FDiscovery->discoInfo(streamJid(),roomJid()));
			}
		}
		else if (isConnected() && !isConnecting)
		{
			emit chatAboutToDisconnect();
		}

		if (FStanzaProcessor->sendStanzaOut(FStreamJid,presence))
		{
			LOG_STRM_INFO(FStreamJid,QString("Presence sent to conference, room=%1, show=%2").arg(FRoomJid.bare()).arg(AShow));
			FConnected = isConnecting;
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send presence to conference, room=%1, show=%2").arg(FRoomJid.bare()).arg(AShow));
		}
	}
	else
	{
		REPORT_ERROR("Failed to send presence to conference: Required interfaces not found");
	}
	return false;
}

bool MultiUserChat::sendMessage(const Message &AMessage, const QString &AToNick)
{
	if (isOpen())
	{
		Jid toJid = FRoomJid;
		toJid.setResource(AToNick);

		Message message = AMessage;
		message.setTo(toJid.full()).setType(AToNick.isEmpty() ? Message::GroupChat : Message::Chat);

		if (FIsolated && FStanzaProcessor && FStanzaProcessor->sendStanzaOut(FStreamJid,message.stanza()))
		{
			emit messageSent(message);
			return true;
		}
		else if (!FIsolated && FMessageProcessor && FMessageProcessor->sendMessage(FStreamJid,message,IMessageProcessor::DirectionOut))
		{
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send message to conference, room=%1").arg(FRoomJid.bare()));
		}
	}
	return false;
}

bool MultiUserChat::sendInvitation(const Jid &AContactJid, const QString &AMessage)
{
	if (FStanzaProcessor && isOpen())
	{
		Message message;
		message.setTo(FRoomJid.bare());

		Stanza &mstanza = message.stanza();
		QDomElement invElem = mstanza.addElement("x",NS_MUC_USER).appendChild(mstanza.createElement("invite")).toElement();
		invElem.setAttribute("to",AContactJid.full());
		
		if (!AMessage.isEmpty())
			invElem.appendChild(mstanza.createElement("reason")).appendChild(mstanza.createTextNode(AMessage));

		if (FStanzaProcessor->sendStanzaOut(FStreamJid, mstanza))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference invite sent, room=%1, contact=%2").arg(FRoomJid.bare(),AContactJid.full()));
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send conference invite, room=%1, contact=%2").arg(FRoomJid.bare(),AContactJid.full()));
		}
	}
	return false;
}

bool MultiUserChat::sendVoiceRequest()
{
	if (FStanzaProcessor && isOpen() && FMainUser->role()==MUC_ROLE_VISITOR)
	{
		Message message;
		message.setTo(FRoomJid.bare()).setId(FStanzaProcessor->newId());

		Stanza &mstanza = message.stanza();
		QDomElement formElem = mstanza.addElement("x",NS_JABBER_DATA);
		formElem.setAttribute("type",DATAFORM_TYPE_SUBMIT);

		QDomElement fieldElem =  formElem.appendChild(mstanza.createElement("field")).toElement();
		fieldElem.setAttribute("var","FORM_TYPE");
		fieldElem.setAttribute("type",DATAFIELD_TYPE_HIDDEN);
		fieldElem.appendChild(mstanza.createElement("value")).appendChild(mstanza.createTextNode(DFT_MUC_REQUEST));

		fieldElem = formElem.appendChild(mstanza.createElement("field")).toElement();
		fieldElem.setAttribute("var","muc#role");
		fieldElem.setAttribute("type",DATAFIELD_TYPE_TEXTSINGLE);
		fieldElem.setAttribute("label","Requested role");
		fieldElem.appendChild(mstanza.createElement("value")).appendChild(mstanza.createTextNode(MUC_ROLE_PARTICIPANT));

		if (FStanzaProcessor->sendStanzaOut(FStreamJid, mstanza))
		{
			LOG_STRM_INFO(FStreamJid,QString("Voice request sent to conference, room=%1").arg(FRoomJid.bare()));
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send voice request to conference, room=%1").arg(FRoomJid.bare()));
		}
	}
	return false;
}

QString MultiUserChat::subject() const
{
	return FSubject;
}

bool MultiUserChat::sendSubject(const QString &ASubject)
{
	if (FStanzaProcessor && isOpen())
	{
		Message message;
		message.setTo(FRoomJid.bare()).setType(Message::GroupChat).setSubject(ASubject);
		if (FStanzaProcessor->sendStanzaOut(FStreamJid,message.stanza()))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference subject message sent, room=%1").arg(FRoomJid.bare()));
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send conference subject message, room=%1").arg(FRoomJid.bare()));
		}
	}
	return false;
}

bool MultiUserChat::sendVoiceApproval(const Message &AMessage)
{
	if (FStanzaProcessor && isOpen())
	{
		Message message = AMessage;
		message.setTo(FRoomJid.bare());

		if (FStanzaProcessor->sendStanzaOut(FStreamJid,message.stanza()))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference voice approval sent, room=%1").arg(FRoomJid.bare()));
			return true;
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send conference voice approval, room=%1").arg(FRoomJid.bare()));
		}
	}
	return false;
}

QString MultiUserChat::loadAffiliationList(const QString &AAffiliation)
{
	if (FStanzaProcessor && isOpen() && AAffiliation!=MUC_AFFIL_NONE)
	{
		Stanza request("iq");
		request.setTo(FRoomJid.bare()).setType("get").setId(FStanzaProcessor->newId());

		QDomElement itemElem = request.addElement("query",NS_MUC_ADMIN).appendChild(request.createElement("item")).toElement();
		itemElem.setAttribute("affiliation",AAffiliation);

		if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_LIST_TIMEOUT))
		{
			LOG_STRM_INFO(FStreamJid,QString("Load affiliation list request sent, room=%1, affiliation=%2, id=%3").arg(FRoomJid.bare(),AAffiliation,request.id()));
			FAffilListLoadId.insert(request.id(), AAffiliation);
			return request.id();
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send load affiliation list request, room=%1, affiliation=%2").arg(FRoomJid.bare(),AAffiliation));
		}
	}
	return QString::null;
}

QString MultiUserChat::updateAffiliationList(const QList<IMultiUserListItem> &AItems)
{
	if (FStanzaProcessor && isOpen() && !AItems.isEmpty())
	{
		Stanza request("iq");
		request.setTo(FRoomJid.bare()).setType("set").setId(FStanzaProcessor->newId());

		QDomElement query = request.addElement("query",NS_MUC_ADMIN);
		foreach(const IMultiUserListItem &item, AItems)
		{
			QDomElement itemElem = query.appendChild(request.createElement("item")).toElement();
			itemElem.setAttribute("jid",item.realJid.full());
			if (!item.reason.isEmpty())
				itemElem.appendChild(request.createElement("reason")).appendChild(request.createTextNode(item.reason));
			itemElem.setAttribute("affiliation",item.affiliation);
		}

		if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_LIST_TIMEOUT))
		{
			LOG_STRM_INFO(FStreamJid,QString("Update affiliation list request sent, room=%1, id=%2").arg(FRoomJid.bare(),request.id()));
			FAffilListUpdateId.insert(request.id(),AItems);
			return request.id();
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send update affiliation list request, room=%1").arg(FRoomJid.bare()));
		}
	}
	return QString::null;
}

QString MultiUserChat::setUserRole(const QString &ANick, const QString &ARole, const QString &AReason)
{
	if (FStanzaProcessor && isOpen())
	{
		IMultiUser *user = findUser(ANick);
		if (user)
		{
			Stanza request("iq");
			request.setTo(FRoomJid.bare()).setType("set").setId(FStanzaProcessor->newId());

			QDomElement itemElem = request.addElement("query",NS_MUC_ADMIN).appendChild(request.createElement("item")).toElement();
			itemElem.setAttribute("role",ARole);
			itemElem.setAttribute("nick",ANick);

			if (!AReason.isEmpty())
				itemElem.appendChild(request.createElement("reason")).appendChild(request.createTextNode(AReason));

			if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_IQ_TIMEOUT))
			{
				LOG_STRM_INFO(FStreamJid,QString("Update role request sent, room=%1, user=%2, role=%3").arg(FRoomJid.bare(),ANick,ARole));
				FRoleUpdateId.insert(request.id(),ANick);
				return request.id();
			}
			else
			{
				LOG_STRM_WARNING(FStreamJid,QString("Failed to send update role request, room=%1, user=%1, role=%3").arg(FRoomJid.bare(),ANick,ARole));
			}
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to change user role, room=%1, user=%2: User not found").arg(FRoomJid.bare(),ANick));
		}
	}
	return QString::null;
}

QString MultiUserChat::setUserAffiliation(const QString &ANick, const QString &AAffiliation, const QString &AReason)
{
	if (FStanzaProcessor && isOpen())
	{
		IMultiUser *user = findUser(ANick);
		if (user)
		{
			Stanza request("iq");
			request.setTo(FRoomJid.bare()).setType("set").setId(FStanzaProcessor->newId());

			QDomElement itemElem = request.addElement("query",NS_MUC_ADMIN).appendChild(request.createElement("item")).toElement();
			itemElem.setAttribute("affiliation",AAffiliation);
			itemElem.setAttribute("nick",ANick);

			if (user->realJid().isValid())
				itemElem.setAttribute("jid",user->realJid().bare());

			if (!AReason.isEmpty())
				itemElem.appendChild(request.createElement("reason")).appendChild(request.createTextNode(AReason));

			if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_IQ_TIMEOUT))
			{
				LOG_STRM_INFO(FStreamJid,QString("Update affiliation request sent, room=%1, user=%2, affiliation=%3").arg(FRoomJid.bare(),ANick,AAffiliation));
				FAffilUpdateId.insert(request.id(),ANick);
				return request.id();
			}
			else
			{
				LOG_STRM_WARNING(FStreamJid,QString("Failed to send update affiliation request, room=%1, user=%2, affiliation=%3").arg(FRoomJid.bare(),ANick,AAffiliation));
			}
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to change user affiliation, room=%1, user=%2: User not found").arg(FRoomJid.bare(),ANick));
		}
	}
	return QString::null;
}

QString MultiUserChat::loadRoomConfig()
{
	if (FStanzaProcessor && isOpen())
	{
		Stanza request("iq");
		request.setTo(FRoomJid.bare()).setType("get").setId(FStanzaProcessor->newId());
		request.addElement("query",NS_MUC_OWNER);

		if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_IQ_TIMEOUT))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference configuration load request sent, room=%1, id=%2").arg(FRoomJid.bare(),request.id()));
			FConfigLoadId.append(request.id());
			return request.id();
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send load conference configuration request, room=%1").arg(FRoomJid.bare()));
		}
	}
	return QString::null;
}

QString MultiUserChat::updateRoomConfig(const IDataForm &AForm)
{
	if (FStanzaProcessor && FDataForms && isOpen())
	{
		Stanza request("iq");
		request.setTo(FRoomJid.bare()).setType("set").setId(FStanzaProcessor->newId());

		QDomElement queryElem = request.addElement("query",NS_MUC_OWNER).toElement();
		FDataForms->xmlForm(AForm,queryElem);

		if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_IQ_TIMEOUT))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference configuration update request sent, room=%1, id=%2").arg(FRoomJid.bare(),request.id()));
			FConfigUpdateId.insert(request.id(),AForm);
			return request.id();
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send update conference configuration request, room=%1").arg(FRoomJid.bare()));
		}
	}
	return QString::null;
}

QString MultiUserChat::destroyRoom(const QString &AReason)
{
	if (FStanzaProcessor && isOpen())
	{
		Stanza request("iq");
		request.setTo(FRoomJid.bare()).setType("set").setId(FStanzaProcessor->newId());

		QDomElement destroyElem = request.addElement("query",NS_MUC_OWNER).appendChild(request.createElement("destroy")).toElement();
		destroyElem.setAttribute("jid",FRoomJid.bare());

		if (!AReason.isEmpty())
			destroyElem.appendChild(request.createElement("reason")).appendChild(request.createTextNode(AReason));

		if (FStanzaProcessor->sendStanzaRequest(this,FStreamJid,request,MUC_IQ_TIMEOUT))
		{
			LOG_STRM_INFO(FStreamJid,QString("Conference destruction request sent, room=%1, id=%2").arg(FRoomJid.bare(),request.id()));
			FRoomDestroyId.insert(request.id(),AReason);
			return request.id();
		}
		else
		{
			LOG_STRM_WARNING(FStreamJid,QString("Failed to send conference destruction request, room=%1").arg(FRoomJid.bare()));
		}
	}
	return false;
}

bool MultiUserChat::processMessage(const Stanza &AStanza)
{
	bool hooked = false;

	Jid fromJid = AStanza.from();
	QString fromNick = fromJid.resource();
	FStatusCodes = statusCodes(AStanza);

	Message message(AStanza);
	if (message.type()==Message::GroupChat && !message.stanza().firstElement("subject").isNull())
	{
		hooked = true;
		QString newSubject = message.subject();
		if (FSubject != newSubject)
		{
			FSubject = newSubject;
			LOG_STRM_INFO(FStreamJid,QString("Conference subject changed, room=%1, user=%2").arg(FRoomJid.bare(),fromNick));
			emit subjectChanged(fromNick, FSubject);
		}
	}
	else if (message.type()!=Message::Error && fromNick.isEmpty())
	{
		if (!AStanza.firstElement("x",NS_MUC_USER).firstChildElement("decline").isNull())
		{
			hooked = true;
			QDomElement declElem = AStanza.firstElement("x",NS_MUC_USER).firstChildElement("decline");
			Jid contactJid = declElem.attribute("from");
			QString reason = declElem.firstChildElement("reason").text();
			LOG_STRM_INFO(FStreamJid,QString("Conference invite declined, room=%1, user=%2: %3").arg(FRoomJid.bare(),contactJid.full(),reason));
			emit invitationDeclined(contactJid,reason);
		}
		else if (FDataForms && !AStanza.firstElement("x",NS_JABBER_DATA).isNull())
		{
			IDataForm form = FDataForms->dataForm(AStanza.firstElement("x",NS_JABBER_DATA));
			if (FDataForms->fieldValue("FORM_TYPE",form.fields) == DFT_MUC_REQUEST)
			{
				hooked = true;
				LOG_STRM_INFO(FStreamJid,QString("Conference voice request received, room=%1").arg(FRoomJid.bare()));
				emit voiceRequestReceived(message);
			}
		}
	}

	if (!hooked && FIsolated)
	{
		hooked = true;
		emit messageReceived(message);
	}

	FStatusCodes.clear();
	return hooked;
}

bool MultiUserChat::processPresence(const Stanza &AStanza)
{
	bool accepted = false;

	Jid fromJid = AStanza.from();
	QString fromNick = fromJid.resource();
	FStatusCodes = statusCodes(AStanza);

	QDomElement xelem = AStanza.firstElement("x",NS_MUC_USER);
	QDomElement itemElem = xelem.firstChildElement("item");

	if (AStanza.type().isEmpty())
	{
		if (!fromNick.isEmpty() && !itemElem.isNull())
		{
			QString role = itemElem.attribute("role");
			QString affiliation = itemElem.attribute("affiliation");

			IPresenceItem presence;
			presence.itemJid = fromJid;
			presence.status = AStanza.firstElement("status").text();
			presence.priority = AStanza.firstElement("priority").text().toInt();

			QString showText = AStanza.firstElement("show").text();
			if (showText.isEmpty())
				presence.show = IPresence::Online;
			else if (showText == "chat")
				presence.show = IPresence::Chat;
			else if (showText == "away")
				presence.show = IPresence::Away;
			else if (showText == "dnd")
				presence.show = IPresence::DoNotDisturb;
			else if (showText == "xa")
				presence.show = IPresence::ExtendedAway;
			else
				presence.show = IPresence::Online;

			QDomElement delayElem = AStanza.firstElement("delay",NS_XMPP_DELAY);
			if (!delayElem.isNull())
				presence.sentTime = DateTime(delayElem.attribute("stamp")).toLocal();
			else
				presence.sentTime = QDateTime::currentDateTime();

			MultiUser *user = FUsers.value(fromNick);
			if (user == NULL)
			{
				Jid realJid = itemElem.attribute("jid");
				user = new MultiUser(FStreamJid,fromJid,realJid,this);
				LOG_STRM_DEBUG(FStreamJid,QString("User entered the conference, room=%1, user=%2, role=%3, affiliation=%4").arg(FRoomJid.bare(),fromNick,role,affiliation));
			}
			else
			{
				if (user->role() != role)
					LOG_STRM_DEBUG(FStreamJid,QString("User role changed, room=%1, user=%2, role=%3").arg(FRoomJid.bare(),fromNick,role));
				if (user->affiliation() != affiliation)
					LOG_STRM_DEBUG(FStreamJid,QString("User affiliation changed, room=%1, user=%2, affiliation=%3").arg(FRoomJid.bare(),fromNick,affiliation));
			}

			user->setRole(role);
			user->setAffiliation(affiliation);
			user->setPresence(presence);

			if (!FUsers.contains(fromNick))
			{
				FUsers.insert(fromNick,user);
				connect(user->instance(),SIGNAL(changed(int, const QVariant &)),SLOT(onUserChanged(int, const QVariant &)));
				emit userChanged(user,MUDR_PRESENCE,QVariant());
			}

			if (!isOpen() && (fromNick==FNickName || FStatusCodes.contains(MUC_SC_SELF_PRESENCE)))
			{
				FMainUser = user;
				if (FNickName != fromNick)
				{
					LOG_STRM_WARNING(FStreamJid,QString("Main user nick was changed by server, room=%1, from=%2, to=%3").arg(FRoomJid.bare(),FNickName,fromNick));
					FNickName = fromNick;
				}
				LOG_STRM_INFO(FStreamJid,QString("Received initial conference presence, room=%1, nick=%2, role=%3, affiliation=%4").arg(FRoomJid.bare(),fromNick,role,affiliation));
				emit chatOpened();
			}

			LOG_STRM_DEBUG(FStreamJid,QString("User changed status in conference, room=%1, user=%2, show=%3, status=%4").arg(FRoomJid.bare(),fromNick).arg(presence.show).arg(presence.status));

			if (user == FMainUser)
			{
				FRoomPresence = presence;
				emit presenceChanged(FRoomPresence);
			}
			accepted = true;
		}
	}
	else if (AStanza.type() == "unavailable")
	{
		MultiUser *user = FUsers.value(fromNick);
		if (user)
		{
			bool applyPresence = true;

			IPresenceItem presence;
			presence.itemJid = fromJid;
			presence.status = AStanza.firstElement("status").text();

			QString role = itemElem.attribute("role");
			QString affiliation = itemElem.attribute("affiliation");

			if (FStatusCodes.contains(MUC_SC_NICK_CHANGED))
			{
				QString newNick = itemElem.attribute("nick");
				if (!newNick.isEmpty())
				{
					LOG_STRM_DEBUG(FStreamJid,QString("Changing user nick from=%1 to=%2 in room=%3").arg(fromNick,newNick,FRoomJid.bare()));
					if (!FUsers.contains(newNick))
					{
						applyPresence = false;
						FUsers.remove(fromNick);
						FUsers.insert(newNick,user);
					}
					user->setNick(newNick);

					if (user == FMainUser)
					{
						FNickName = newNick;
						sendPresence(FRoomPresence.show,FRoomPresence.status,FRoomPresence.priority);
					}
				}
				else
				{
					LOG_STRM_ERROR(FStreamJid,QString("Failed to changes user nick, room=%1, user=%2: New nick is empty").arg(FRoomJid.bare(),fromNick));
				}
			}
			else if (FStatusCodes.contains(MUC_SC_USER_KICKED))
			{
				QString actor = itemElem.firstChildElement("actor").attribute("nick");
				QString reason = itemElem.firstChildElement("reason").text();
				LOG_STRM_DEBUG(FStreamJid,QString("User was kicked from conference, room=%1, user=%2, by=%3: %4").arg(FRoomJid.bare(),fromNick,actor,reason));
				emit userKicked(fromNick,reason,actor);
			}
			else if (FStatusCodes.contains(MUC_SC_USER_BANNED))
			{
				QString actor = itemElem.firstChildElement("actor").attribute("nick");
				QString reason = itemElem.firstChildElement("reason").text();
				LOG_STRM_DEBUG(FStreamJid,QString("User was banned in conference, room=%1, user=%2, by=%3: %4").arg(FRoomJid.bare(),fromNick,actor,reason));
				emit userBanned(fromNick,reason,actor);
			}
			else if (!xelem.firstChildElement("destroy").isNull())
			{
				QString reason = xelem.firstChildElement("destroy").firstChildElement("reason").text();
				LOG_STRM_DEBUG(FStreamJid,QString("Conference was destroyed by owner, room=%1: %2").arg(FRoomJid.bare(),reason));
				emit roomDestroyed(QString::null,reason);
			}

			if (applyPresence)
			{
				user->setRole(role);
				user->setAffiliation(affiliation);
				user->setPresence(presence);

				if (user != FMainUser)
				{
					LOG_STRM_DEBUG(FStreamJid,QString("User leave the conference, room=%1, user=%2, show=%3, status=%4").arg(FRoomJid.bare(),fromNick).arg(presence.show).arg(presence.status));
					FUsers.remove(fromNick);
					delete user;
				}
				else
				{
					LOG_STRM_INFO(FStreamJid,QString("You leave the conference, room=%1, show=%2, status=%3").arg(FRoomJid.bare(),role,affiliation));
					closeChat(presence);
				}
			}

			accepted = true;
		}
	}
	else if (AStanza.type() == "error")
	{
		XmppStanzaError err(AStanza);
		LOG_STRM_WARNING(FStreamJid,QString("Error presence received in conference, room=%1, user=%2: %3").arg(FRoomJid.bare(),fromNick,err.condition()));

		if (fromNick == FNickName)
		{
			FRoomError = err;

			IPresenceItem presence;
			presence.itemJid = fromJid;
			presence.show = IPresence::Error;
			presence.status = err.errorMessage();
			closeChat(presence);
		}

		accepted = true;
	}

	FStatusCodes.clear();
	return accepted;
}

void MultiUserChat::closeChat(const IPresenceItem &APresence)
{
	FConnected = false;
	LOG_STRM_INFO(FStreamJid,QString("Closing conference, room=%1").arg(FRoomJid.bare()));

	if (isOpen())
	{
		FMainUser->setPresence(APresence);
		delete FMainUser;
	}
	FMainUser = NULL;
	FUsers.remove(FNickName);

	foreach(MultiUser *user, FUsers)
		user->setPresence(IPresenceItem());
	qDeleteAll(FUsers);
	FUsers.clear();

	FRoomPresence = APresence;
	emit presenceChanged(FRoomPresence);

	emit chatClosed();
}

void MultiUserChat::onUserChanged(int AData, const QVariant &ABefore)
{
	IMultiUser *user = qobject_cast<IMultiUser *>(sender());
	if (user)
		emit userChanged(user,AData,ABefore);
}

void MultiUserChat::onDiscoveryInfoReceived(const IDiscoInfo &AInfo)
{
	if (AInfo.streamJid==streamJid() && AInfo.contactJid==roomJid())
	{
		int index = FDiscovery->findIdentity(AInfo.identity,"conference","text");
		QString name = index>=0 ? AInfo.identity.at(index).name : QString::null;
		if (!name.isEmpty() && FRoomName!=name)
		{
			FRoomName = name;
			LOG_STRM_DEBUG(FStreamJid,QString("Conference name changed, room=%1: %2").arg(FRoomJid.bare(),FRoomName));
			emit roomNameChanged(FRoomName);
		}
	}
}

void MultiUserChat::onXmppStreamClosed(IXmppStream *AXmppStream)
{
	if (AXmppStream->streamJid() == FStreamJid)
	{
		if (!FUsers.isEmpty())
			closeChat(IPresenceItem());
	}
}

void MultiUserChat::onXmppStreamJidChanged(IXmppStream *AXmppStream, const Jid &ABefore)
{
	if (ABefore == FStreamJid)
	{
		FStreamJid = AXmppStream->streamJid();
		emit streamJidChanged(ABefore,FStreamJid);
	}
}

void MultiUserChat::onPresenceChanged(IPresence *APresence, int AShow, const QString &AStatus, int APriority)
{
	if (APresence->streamJid() == FStreamJid) 
	{
		if (FAutoPresence)
			sendPresence(AShow,AStatus,APriority);
	}
}
