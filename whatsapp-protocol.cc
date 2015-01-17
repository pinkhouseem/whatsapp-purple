
/*
 * WhatsApp API implementation in C++ for libpurple.
 * Written by David Guillen Fandos (david@davidgf.net) based 
 * on the sources of WhatsAPI PHP implementation.
 * v1.4 changes based on WP7 sources
 *
 * Share and enjoy!
 *
 */

#include <iostream>
#include <map>
#include <vector>
#include <map>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#ifdef ENABLE_OPENSSL
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#else
#include "wa_api.h"
#endif

#include "wadict.h"
#include "rc4.h"
#include "keygen.h"
#include "databuffer.h"
#include "tree.h"
#include "contacts.h"
#include "message.h"
#include "wa_connection.h"
#include "thumb.h"

#define MESSAGE_CHAT     0
#define MESSAGE_IMAGE    1
#define MESSAGE_LOCATION 2

DataBuffer WhatsappConnection::generateResponse(std::string from, std::string type, std::string id)
{
	if (type == "") { // Auto 
		if (sendRead) type = "read";
		else type = "delivery";
	}
	Tree mes("receipt", makeAttr4("to", from, "id", id, "type", type, "t", int2str(1)));

	return serialize_tree(&mes);
}

/* Send image transaction */
int WhatsappConnection::sendImage(std::string to, int w, int h, unsigned int size, const char *fp)
{
	/* Type can be: audio/image/video */
	std::string sha256b64hash = SHA256_file_b64(fp);
	Tree iq("media", makeAttr3("type", "image", "hash", sha256b64hash, "size", int2str(size)));
	Tree req("iq", makeAttr4("id", int2str(++iqid), "type", "set", "to", whatsappserver, "xmlns", "w:m"));
	req.addChild(iq);

	t_fileupload fu;
	fu.to = to;
	fu.file = std::string(fp);
	fu.rid = iqid;
	fu.hash = sha256b64hash;
	fu.type = "image";
	fu.uploading = false;
	fu.totalsize = 0;
	uploadfile_queue.push_back(fu);
	outbuffer = outbuffer + serialize_tree(&req);

	return iqid;
}

WhatsappConnection::WhatsappConnection(std::string phone, std::string password, std::string nickname)
{
	this->phone = phone;
	this->password = password;
	this->in = NULL;
	this->out = NULL;
	this->conn_status = SessionNone;
	this->msgcounter = 1;
	this->iqid = 1;
	this->nickname = nickname;
	this->whatsappserver = "s.whatsapp.net";
	this->whatsappservergroup = "g.us";
	this->mypresence = "available";
	this->groups_updated = false;
	this->gq_stat = 0;
	this->gw1 = -1;
	this->gw2 = -1;
	this->gw3 = 0;
	this->sslstatus = 0;
	this->frame_seq = 0;
	this->sendRead = true;

	/* Trim password spaces */
	while (password.size() > 0 and password[0] == ' ')
		password = password.substr(1);
	while (password.size() > 0 and password[password.size() - 1] == ' ')
		password = password.substr(0, password.size() - 1);
}

WhatsappConnection::~WhatsappConnection()
{
	if (this->in)
		delete this->in;
	if (this->out)
		delete this->out;
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		delete recv_messages[i];
	}
}

std::map < std::string, Group > WhatsappConnection::getGroups()
{
	return groups;
}

bool WhatsappConnection::groupsUpdated()
{
	if (gq_stat == 7) {
		groups_updated = true;
		gq_stat = 8;
	}

	if (groups_updated and gw3 <= 0) {
		groups_updated = false;
		return true;
	}

	return false;
}

void WhatsappConnection::updateGroups()
{
	/* Get the group list */
	groups.clear();
	{
		gw1 = iqid;
		Tree iq("list", makeAttr1("type", "owning"));
		Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", "g.us", "xmlns", "w:g"));
		req.addChild(iq);
		outbuffer = outbuffer + serialize_tree(&req);
	}
	{
		gw2 = iqid;
		Tree iq("list", makeAttr1("type", "participating"));
		Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", "g.us", "xmlns", "w:g"));
		req.addChild(iq);
		outbuffer = outbuffer + serialize_tree(&req);
	}
	gq_stat = 1;		/* Queried the groups */
	gw3 = 0;
}

void WhatsappConnection::manageParticipant(std::string group, std::string participant, std::string command)
{
	Tree part("participant", makeAttr1("jid", participant));
	Tree iq(command);
	iq.addChild(part);
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "set", "to", group + "@g.us", "xmlns", "w:g"));
	req.addChild(iq);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::leaveGroup(std::string group)
{
	Tree gr("group", makeAttr1("id", group + "@g.us"));
	Tree iq("leave");
	iq.addChild(gr);
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "set", "to", "g.us", "xmlns", "w:g"));
	req.addChild(iq);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::addGroup(std::string subject)
{
	Tree gr("group", makeAttr2("action", "create", "subject", subject));
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "set", "to", "g.us", "xmlns", "w:g"));
	req.addChild(gr);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::doLogin(std::string resource)
{
	/* Send stream init */
	DataBuffer first;

	{
		std::map < std::string, std::string > auth;
		first.addData("WA\1\5", 4);
		auth["resource"] = resource;
		auth["to"] = whatsappserver;
		Tree t("start", auth);
		first = first + serialize_tree(&t, false);
	}

	/* Send features */
	{
		Tree p("stream:features");
		p.addChild(Tree("readreceipts"));
		first = first + serialize_tree(&p, false);
	}

	/* Send auth request */
	{
		std::map < std::string, std::string > auth;
		auth["mechanism"] = "WAUTH-2";
		auth["user"] = phone;
		Tree t("auth", auth);
		t.forceDataWrite();
		first = first + serialize_tree(&t, false);
	}

	conn_status = SessionWaitingChallenge;
	outbuffer = first;
}

void WhatsappConnection::receiveCallback(const char *data, int len)
{
	if (data != NULL and len > 0)
		inbuffer.addData(data, len);
	this->processIncomingData();
}

int WhatsappConnection::sendCallback(char *data, int len)
{
	int minlen = outbuffer.size();
	if (minlen > len)
		minlen = len;

	memcpy(data, outbuffer.getPtr(), minlen);
	return minlen;
}

bool WhatsappConnection::hasDataToSend()
{
	return outbuffer.size() != 0;
}

void WhatsappConnection::sentCallback(int len)
{
	outbuffer.popData(len);
}

int WhatsappConnection::sendSSLCallback(char *buffer, int maxbytes)
{
	int minlen = sslbuffer.size();
	if (minlen > maxbytes)
		minlen = maxbytes;

	memcpy(buffer, sslbuffer.getPtr(), minlen);
	return minlen;
}

int WhatsappConnection::sentSSLCallback(int bytessent)
{
	sslbuffer.popData(bytessent);
	return bytessent;
}

void WhatsappConnection::receiveSSLCallback(char *buffer, int bytesrecv)
{
	if (buffer != NULL and bytesrecv > 0)
		sslbuffer_in.addData(buffer, bytesrecv);
	this->processSSLIncomingData();
}

bool WhatsappConnection::hasSSLDataToSend()
{
	return sslbuffer.size() != 0;
}

bool WhatsappConnection::closeSSLConnection()
{
	return sslstatus == 0;
}

void WhatsappConnection::SSLCloseCallback()
{
	sslstatus = 0;
}

bool WhatsappConnection::hasSSLConnection(std::string & host, int *port)
{
	host = "";
	*port = 443;

	if (sslstatus == 1)
		for (unsigned int j = 0; j < uploadfile_queue.size(); j++)
			if (uploadfile_queue[j].uploading) {
				host = uploadfile_queue[j].host;
				return true;
			}

	return false;
}

int WhatsappConnection::uploadProgress(int &rid, int &bs)
{
	if (!(sslstatus == 1 or sslstatus == 2))
		return 0;
	int totalsize = 0;
	for (unsigned int j = 0; j < uploadfile_queue.size(); j++)
		if (uploadfile_queue[j].uploading) {
			rid = uploadfile_queue[j].rid;
			totalsize = uploadfile_queue[j].totalsize;
			break;
		}
	bs = totalsize - sslbuffer.size();
	if (bs < 0)
		bs = 0;
	return 1;
}

int WhatsappConnection::uploadComplete(int rid) {
	for (unsigned int j = 0; j < uploadfile_queue.size(); j++)
		if (rid == uploadfile_queue[j].rid)
			return 0;

	return 1;
}

void WhatsappConnection::subscribePresence(std::string user)
{
	Tree request("presence", makeAttr2("type", "subscribe", "to", user));
	outbuffer = outbuffer + serialize_tree(&request);
}

void WhatsappConnection::queryStatuses()
{
	Tree req("iq", makeAttr4("to", "s.whatsapp.net", "type", "get", "id", int2str(iqid++), "xmlns", "status"));
	Tree stat("status");

	for (std::map < std::string, Contact >::iterator iter = contacts.begin(); iter != contacts.end(); iter++)
	{
		Tree user("user", makeAttr1("jid", iter->first + "@" + whatsappserver));
		stat.addChild(user);
	}
	req.addChild(stat);
	
	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::getLast(std::string user)
{
	Tree iq("query");
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", user, "xmlns", "jabber:iq:last"));
	req.addChild(iq);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::gotTyping(std::string who, std::string tstat)
{
	who = getusername(who);
	if (contacts.find(who) != contacts.end()) {
		contacts[who].typing = tstat;
		user_typing.push_back(who);
	}
}

void WhatsappConnection::notifyTyping(std::string who, int status)
{
	std::string s = "paused";
	if (status == 1)
		s = "composing";

	Tree mes("chatstate", makeAttr1("to", who + "@" + whatsappserver));
	mes.addChild(Tree(s));

	outbuffer = outbuffer + serialize_tree(&mes);
}

void WhatsappConnection::account_info(unsigned long long &creation, unsigned long long &freeexp, std::string & status)
{
	creation = str2lng(account_creation);
	freeexp = str2lng(account_expiration);
	status = account_status;
}

void WhatsappConnection::queryPreview(std::string user)
{
	Tree pic("picture", makeAttr1("type", "preview"));
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", user, "xmlns", "w:profile:picture"));
	req.addChild(pic);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::queryFullSize(std::string user)
{
	Tree pic("picture");
	Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", user, "xmlns", "w:profile:picture"));
	req.addChild(pic);

	outbuffer = outbuffer + serialize_tree(&req);
}

void WhatsappConnection::send_avatar(const std::string & avatar)
{
	Tree pic("picture"); pic.setData(avatar);
	Tree prev("picture", makeAttr1("type", "preview")); prev.setData(avatar);

	Tree req("iq", makeAttr4("id", "set_photo_"+int2str(iqid++), "type", "set", "to", phone + "@" + whatsappserver, "xmlns", "w:profile:picture"));
	req.addChild(pic);
	req.addChild(prev);

	outbuffer = outbuffer + serialize_tree(&req);
}

bool WhatsappConnection::queryReceivedMessage(char *msgid, int * type)
{
	if (received_messages.size() == 0) return false;

	strcpy(msgid, received_messages[0].first.c_str());
	*type = received_messages[0].second;
	received_messages.erase(received_messages.begin());

	return true;
}

void WhatsappConnection::getMessageId(char * msgid)
{
	unsigned int t = time(NULL);
	unsigned int mid = msgcounter++;

	sprintf(msgid, "%u-%u", t, mid);
}

void WhatsappConnection::sendChat(std::string msgid, std::string to, std::string message)
{
	ChatMessage msg(this, to, time(NULL), msgid, message, nickname);
	DataBuffer buf = msg.serialize();

	outbuffer = outbuffer + buf;
}

void WhatsappConnection::sendGroupChat(std::string msgid, std::string to, std::string message)
{
	ChatMessage msg(this, to, time(NULL), msgid, message, nickname);
	msg.server = "g.us";
	DataBuffer buf = msg.serialize();

	outbuffer = outbuffer + buf;
}

void WhatsappConnection::addContacts(std::vector < std::string > clist)
{
	/* Insert the contacts to the contact list */
	for (unsigned int i = 0; i < clist.size(); i++) {
		if (contacts.find(clist[i]) == contacts.end())
			contacts[clist[i]] = Contact(clist[i], true);
		else
			contacts[clist[i]].mycontact = true;

		user_changes.push_back(clist[i]);
	}
	/* Query the profile pictures */
	bool qstatus = false;
	for (std::map < std::string, Contact >::iterator iter = contacts.begin(); iter != contacts.end(); iter++) {
		if (not iter->second.subscribed) {
			iter->second.subscribed = true;

			this->subscribePresence(iter->first + "@" + whatsappserver);
			this->queryPreview(iter->first + "@" + whatsappserver);
			this->getLast(iter->first + "@" + whatsappserver);
			qstatus = true;
		}
	}
	/* Query statuses */
	if (qstatus)
		this->queryStatuses();
}

unsigned char hexchars(char c1, char c2)
{
	if (c1 >= '0' and c1 <= '9')
		c1 -= '0';
	else if (c1 >= 'A' and c1 <= 'F')
		c1 = c1 - 'A' + 10;
	else if (c1 >= 'a' and c1 <= 'f')
		c1 = c1 - 'a' + 10;

	if (c2 >= '0' and c2 <= '9')
		c2 -= '0';
	else if (c2 >= 'A' and c2 <= 'F')
		c2 = c2 - 'A' + 10;
	else if (c2 >= 'a' and c2 <= 'f')
		c2 = c2 - 'a' + 10;

	unsigned char r = c2 | (c1 << 4);
	return r;
}

std::string UnicodeToUTF8(unsigned int c)
{
	std::string ret;
	if (c <= 0x7F)
		ret += ((char)c);
	else if (c <= 0x7FF) {
		ret += ((char)(0xC0 | (c >> 6)));
		ret += ((char)(0x80 | (c & 0x3F)));
	} else if (c <= 0xFFFF) {
		if (c >= 0xD800 and c <= 0xDFFF)
			return ret;	/* Invalid char */
		ret += ((char)(0xE0 | (c >> 12)));
		ret += ((char)(0x80 | ((c >> 6) & 0x3F)));
		ret += ((char)(0x80 | (c & 0x3F)));
	}
	return ret;
}

std::string utf8_decode(std::string in)
{
	std::string dec;
	for (unsigned int i = 0; i < in.size(); i++) {
		if (in[i] == '\\' and in[i + 1] == 'u') {
			i += 2;	/* Skip \u */
			unsigned char hex1 = hexchars(in[i + 0], in[i + 1]);
			unsigned char hex2 = hexchars(in[i + 2], in[i + 3]);
			unsigned int uchar = (hex1 << 8) | hex2;
			dec += UnicodeToUTF8(uchar);
			i += 3;
		} else if (in[i] == '\\' and in[i + 1] == '"') {
			dec += '"';
			i++;
		} else
			dec += in[i];
	}
	return dec;
}

std::string query_field(std::string work, std::string lo, bool integer = false)
{
	size_t p = work.find("\"" + lo + "\"");
	if (p == std::string::npos)
		return "";

	work = work.substr(p + ("\"" + lo + "\"").size());

	p = work.find("\"");
	if (integer)
		p = work.find(":");
	if (p == std::string::npos)
		return "";

	work = work.substr(p + 1);

	p = 0;
	while (p < work.size()) {
		if (work[p] == '"' and(p == 0 or work[p - 1] != '\\'))
			break;
		p++;
	}
	if (integer) {
		p = 0;
		while (p < work.size()and work[p] >= '0' and work[p] <= '9')
			p++;
	}
	if (p == std::string::npos)
		return "";

	work = work.substr(0, p);

	return work;
}

void WhatsappConnection::updateContactStatuses(std::string json)
{
	while (true) {
		size_t offset = json.find("{");
		if (offset == std::string::npos)
			break;
		json = json.substr(offset + 1);

		/* Look for closure */
		size_t cl = json.find("{");
		if (cl == std::string::npos)
			cl = json.size();
		std::string work = json.substr(0, cl);

		/* Look for "n", the number and "w","t","s" */
		std::string n = query_field(work, "n");
		std::string w = query_field(work, "w", true);
		std::string t = query_field(work, "t", true);
		std::string s = query_field(work, "s");

		if (w == "1") {
			contacts[n].status = utf8_decode(s);
			contacts[n].last_status = str2lng(t);
		}

		json = json.substr(cl);
	}
}

void WhatsappConnection::updateFileUpload(std::string json)
{
	size_t offset = json.find("{");
	if (offset == std::string::npos)
		return;
	json = json.substr(offset + 1);

	/* Look for closure */
	size_t cl = json.find("{");
	if (cl == std::string::npos)
		cl = json.size();
	std::string work = json.substr(0, cl);

	std::string url = query_field(work, "url");
	std::string type = query_field(work, "type");
	std::string size = query_field(work, "size");
	std::string width = query_field(work, "width");
	std::string height = query_field(work, "height");
	std::string filehash = query_field(work, "filehash");
	std::string mimetype = query_field(work, "mimetype");

	std::string to;
	for (unsigned int j = 0; j < uploadfile_queue.size(); j++)
		if (uploadfile_queue[j].uploading and uploadfile_queue[j].hash == filehash) {
			to = uploadfile_queue[j].to;
			uploadfile_queue.erase(uploadfile_queue.begin() + j);
			break;
		}
	/* Send the message with the URL :) */
	ImageMessage msg(this, to, time(NULL), int2str(msgcounter++), "author", url, str2int(width), str2int(height), str2int(size), "encoding", filehash, mimetype, temp_thumbnail);

	DataBuffer buf = msg.serialize();

	outbuffer = outbuffer + buf;
}

/* Quick and dirty way to parse the HTTP responses */
void WhatsappConnection::processSSLIncomingData()
{
	/* Parse HTTPS headers and JSON body */
	if (sslstatus == 1)
		sslstatus++;

	if (sslstatus == 2) {
		/* Look for the first line, to be 200 OK */
		std::string toparse((char *)sslbuffer_in.getPtr(), sslbuffer_in.size());
		if (toparse.find("\r\n") != std::string::npos) {
			std::string fl = toparse.substr(0, toparse.find("\r\n"));
			if (fl.find("200") == std::string::npos)
				goto abortStatus;

			if (toparse.find("\r\n\r\n") != std::string::npos) {
				std::string headers = toparse.substr(0, toparse.find("\r\n\r\n") + 4);
				std::string content = toparse.substr(toparse.find("\r\n\r\n") + 4);

				/* Look for content length */
				if (headers.find("Content-Length:") != std::string::npos) {
					std::string clen = headers.substr(headers.find("Content-Length:") + strlen("Content-Length:"));
					clen = clen.substr(0, clen.find("\r\n"));
					while (clen.size() > 0 and clen[0] == ' ')
						clen = clen.substr(1);
					unsigned int contentlength = str2int(clen);
					if (contentlength == content.size()) {
						/* Now we can proceed to parse the JSON */
						updateFileUpload(content);
						sslstatus = 0;
					}
				}
			}
		}
	}

	processUploadQueue();
	return;
abortStatus:
	sslstatus = 0;
	processUploadQueue();
	return;
}

std::string WhatsappConnection::generateUploadPOST(t_fileupload * fu)
{
	std::string file_buffer;
	FILE *fd = fopen(fu->file.c_str(), "rb");
	int read = 0;
	do {
		char buf[1024];
		read = fread(buf, 1, 1024, fd);
		file_buffer += std::string(buf, read);
	} while (read > 0);
	fclose(fd);

	std::string mime_type = std::string(file_mime_type(fu->file.c_str(), file_buffer.c_str(), file_buffer.size()));
	std::string encoded_name = "TODO..:";

	std::string ret;
	/* BODY HEAD */
	ret += "--zzXXzzYYzzXXzzQQ\r\n";
	ret += "Content-Disposition: form-data; name=\"to\"\r\n\r\n";
	ret += fu->to + "\r\n";
	ret += "--zzXXzzYYzzXXzzQQ\r\n";
	ret += "Content-Disposition: form-data; name=\"from\"\r\n\r\n";
	ret += fu->from + "\r\n";
	ret += "--zzXXzzYYzzXXzzQQ\r\n";
	ret += "Content-Disposition: form-data; name=\"file\"; filename=\"" + encoded_name + "\"\r\n";
	ret += "Content-Type: " + mime_type + "\r\n\r\n";

	/* File itself */
	ret += file_buffer;

	/* TAIL */
	ret += "\r\n--zzXXzzYYzzXXzzQQ--\r\n";

	std::string post;
	post += "POST " + fu->uploadurl + "\r\n";
	post += "Content-Type: multipart/form-data; boundary=zzXXzzYYzzXXzzQQ\r\n";
	post += "Host: " + fu->host + "\r\n";
	post += "User-Agent: WhatsApp/2.4.7 S40Version/14.26 Device/Nokia302\r\n";
	post += "Content-Length:  " + int2str(ret.size()) + "\r\n\r\n";

	std::string all = post + ret;

	fu->totalsize = file_buffer.size();

	return all;
}

void WhatsappConnection::processUploadQueue()
{
	/* At idle check for new uploads */
	if (sslstatus == 0) {
		for (unsigned int j = 0; j < uploadfile_queue.size(); j++) {
			if (uploadfile_queue[j].uploadurl != "" and not uploadfile_queue[j].uploading) {
				uploadfile_queue[j].uploading = true;
				std::string postq = generateUploadPOST(&uploadfile_queue[j]);

				sslbuffer_in.clear();
				sslbuffer.clear();

				sslbuffer.addData(postq.c_str(), postq.size());

				sslstatus = 1;
				break;
			}
		}
	}
}

void WhatsappConnection::processIncomingData()
{
	/* Parse the data and create as many Trees as possible */
	std::vector < Tree > treelist;
	try {
		if (inbuffer.size() >= 3) {
			/* Consume as many trees as possible */
			Tree t;
			do {
				t = parse_tree(&inbuffer);
				if (t.getTag() != "treeerr")
					treelist.push_back(t);
			} while (t.getTag() != "treeerr" and inbuffer.size() >= 3);
		}
	}
	catch(int n) {
		printf("In stream error! Need to handle this properly...\n");
		return;
	}

	/* Now process the tree list! */
	for (unsigned int i = 0; i < treelist.size(); i++) {
		DEBUG_PRINT( treelist[i].toString() );
		if (treelist[i].getTag() == "challenge") {
			/* Generate a session key using the challege & the password */
			assert(conn_status == SessionWaitingChallenge);

			KeyGenerator::generateKeysV14(password, treelist[i].getData().c_str(), treelist[i].getData().size(), (char *)this->session_key);

			in  = new RC4Decoder(&session_key[20*2], 20, 768);
			out = new RC4Decoder(&session_key[20*0], 20, 768);

			conn_status = SessionWaitingAuthOK;
			challenge_data = treelist[i].getData();

			this->sendResponse();
		} else if (treelist[i].getTag() == "success") {
			/* Notifies the success of the auth */
			conn_status = SessionConnected;
			if (treelist[i].hasAttribute("status"))
				this->account_status = treelist[i].getAttributes()["status"];
			if (treelist[i].hasAttribute("kind"))
				this->account_type = treelist[i].getAttributes()["kind"];
			if (treelist[i].hasAttribute("expiration"))
				this->account_expiration = treelist[i].getAttributes()["expiration"];
			if (treelist[i].hasAttribute("creation"))
				this->account_creation = treelist[i].getAttributes()["creation"];

			this->notifyMyPresence();
			this->sendInitial();  // Seems to trigger an error IQ response
			this->updateGroups();

			DEBUG_PRINT("Logged in!!!");
		} else if (treelist[i].getTag() == "failure") {
			if (conn_status == SessionWaitingAuthOK)
				this->notifyError(errorAuth);
			else
				this->notifyError(errorUnknown);
		} else if (treelist[i].getTag() == "notification") {
			DataBuffer reply = generateResponse(
				treelist[i].getAttribute("from"),
			    treelist[i].getAttribute("type"),
			    treelist[i].getAttribute("id")
		    );
			outbuffer = outbuffer + reply;
			
			if (treelist[i].hasAttributeValue("type", "participant") || 
				treelist[i].hasAttributeValue("type", "owner") ) {
				/* If the nofitication comes from a group, assume we have to reload groups ;) */
				updateGroups();
			}
		} else if (treelist[i].getTag() == "ack") {
			std::string id = treelist[i].getAttribute("id");
			received_messages.push_back( std::make_pair(id,0) );

		} else if (treelist[i].getTag() == "receipt") {
			std::string id = treelist[i].getAttribute("id");
			std::string type = treelist[i].getAttribute("type");
			if (type == "") type = "delivery";
			
			Tree mes("ack", makeAttr3("class", "receipt", "type", type, "id", id));
			outbuffer = outbuffer + serialize_tree(&mes);

			// Add reception package to queue
			int rtype = 1;
			if (type == "read") rtype = 2;
			received_messages.push_back( std::make_pair(id,rtype) );
			
		} else if (treelist[i].getTag() == "chatstate") {
			if (treelist[i].hasChild("composing"))
				this->gotTyping(treelist[i].getAttribute("from"), "composing");
			if (treelist[i].hasChild("paused"))
				this->gotTyping(treelist[i].getAttribute("from"), "paused");
			
		} else if (treelist[i].getTag() == "message") {
			/* Receives a message! */
			DEBUG_PRINT("Received message stanza...");
			if (treelist[i].hasAttribute("from") and
				(treelist[i].hasAttributeValue("type", "text") or treelist[i].hasAttributeValue("type", "media"))) {
				unsigned long long time = 0;
				if (treelist[i].hasAttribute("t"))
					time = str2lng(treelist[i].getAttribute("t"));
				std::string from = treelist[i].getAttribute("from");
				std::string id = treelist[i].getAttribute("id");
				std::string author = treelist[i].getAttribute("participant");

				Tree t = treelist[i].getChild("body");
				if (t.getTag() != "treeerr") {
					this->receiveMessage(ChatMessage(this, from, time, id, t.getData(), author));
				}
				t = treelist[i].getChild("media");
				if (t.getTag() != "treeerr") {
					if (t.hasAttributeValue("type", "image")) {
						this->receiveMessage(ImageMessage(this, from, time, id, author, t.getAttribute("url"), str2int(t.getAttribute("width")), str2int(t.getAttribute("height")), str2int(t.getAttribute("size")), t.getAttribute("encoding"), t.getAttribute("filehash"), t.getAttribute("mimetype"), t.getData()));
					} else if (t.hasAttributeValue("type", "location")) {
						this->receiveMessage(LocationMessage(this, from, time, id, author, str2dbl(t.getAttribute("latitude")), str2dbl(t.getAttribute("longitude")), t.getData()));
					} else if (t.hasAttributeValue("type", "audio")) {
						this->receiveMessage(SoundMessage(this, from, time, id, author, t.getAttribute("url"), t.getAttribute("filehash"), t.getAttribute("mimetype")));
					} else if (t.hasAttributeValue("type", "video")) {
						this->receiveMessage(VideoMessage(this, from, time, id, author, t.getAttribute("url"), t.getAttribute("filehash"), t.getAttribute("mimetype")));
					}
				}
			} else if (treelist[i].hasAttributeValue("type", "notification") and treelist[i].hasAttribute("from")) {
				/* If the nofitication comes from a group, assume we have to reload groups ;) */
				updateGroups();
			}
			/* Generate response for the messages */
			if (treelist[i].hasAttribute("type") and treelist[i].hasAttribute("from")) { //FIXME
				DataBuffer reply = generateResponse(treelist[i].getAttribute("from"),
								    "",
								    treelist[i].getAttribute("id")
								    );
				outbuffer = outbuffer + reply;
			}
		} else if (treelist[i].getTag() == "presence") {
			/* Receives the presence of the user, for v14 type is optional */
			if (treelist[i].hasAttribute("from")) {
				this->notifyPresence(treelist[i].getAttribute("from"), treelist[i].getAttribute("type"));
			}
		} else if (treelist[i].getTag() == "iq") {
			/* Receives the presence of the user */
			if (atoi(treelist[i].getAttribute("id").c_str()) == gw1)
				gq_stat |= 2;
			if (atoi(treelist[i].getAttribute("id").c_str()) == gw2)
				gq_stat |= 4;

			if (treelist[i].hasAttributeValue("type", "result") and treelist[i].hasAttribute("from")) {
				Tree t = treelist[i].getChild("query");
				if (t.getTag() != "treeerr") {
					if (t.hasAttribute("seconds")) {
						this->notifyLastSeen(treelist[i].getAttribute("from"), t.getAttribute("seconds"));
					}
				}
				t = treelist[i].getChild("picture");
				if (t.getTag() != "treeerr") {
					if (t.hasAttributeValue("type", "preview"))
						this->addPreviewPicture(treelist[i].getAttribute("from"), t.getData());
					if (t.hasAttributeValue("type", "image"))
						this->addFullsizePicture(treelist[i].getAttribute("from"), t.getData());
				}

				t = treelist[i].getChild("media");
				if (t.getTag() != "treeerr") {
					for (unsigned int j = 0; j < uploadfile_queue.size(); j++) {
						if (uploadfile_queue[j].rid == str2int(treelist[i].getAttribute("id"))) {
							/* Queue to upload the file */
							uploadfile_queue[j].uploadurl = t.getAttribute("url");
							std::string host = uploadfile_queue[j].uploadurl.substr(8);	/* Remove https:// */
							for (unsigned int i = 0; i < host.size(); i++)
								if (host[i] == '/')
									host = host.substr(0, i);
							uploadfile_queue[j].host = host;

							this->processUploadQueue();
							break;
						}
					}
				}

				t = treelist[i].getChild("duplicate");
				if (t.getTag() != "treeerr") {
					for (unsigned int j = 0; j < uploadfile_queue.size(); j++) {
						if (uploadfile_queue[j].rid == str2int(treelist[i].getAttribute("id"))) {
							/* Generate a fake JSON and process directly */
							std::string json = "{\"name\":\"" + uploadfile_queue[j].file + "\"," "\"url\":\"" + t.getAttribute("url") + "\"," "\"size\":\"" + t.getAttribute("size") + "\"," "\"mimetype\":\"" + t.getAttribute("mimetype") + "\"," "\"filehash\":\"" + t.getAttribute("filehash") + "\"," "\"type\":\"" + t.getAttribute("type") + "\"," "\"width\":\"" + t.getAttribute("width") + "\"," "\"height\":\"" + t.getAttribute("height") + "\"}";

							uploadfile_queue[j].uploading = true;
							this->updateFileUpload(json);
							break;
						}
					}
				}

				// Status result
				t = treelist[i].getChild("status");
				if (t.getTag() != "treeerr") {
					std::vector < Tree > childs = t.getChildren();
					for (unsigned int j = 0; j < childs.size(); j++) {
						if (childs[j].getTag() == "user") {
							std::string user = getusername(childs[j].getAttribute("jid"));
							contacts[user].status = utf8_decode(childs[j].getData());
						}
					}
				}

				std::vector < Tree > childs = treelist[i].getChildren();
				int acc = 0;
				for (unsigned int j = 0; j < childs.size(); j++) {
					if (childs[j].getTag() == "group") {
						bool rep = groups.find(getusername(childs[j].getAttribute("id"))) != groups.end();
						if (not rep) {
							groups.insert(std::pair < std::string, Group > (getusername(childs[j].getAttribute("id")), Group(getusername(childs[j].getAttribute("id")), childs[j].getAttribute("subject"), getusername(childs[j].getAttribute("owner")))));

							/* Query group participants */
							Tree iq("list");
							Tree req("iq", makeAttr4("id", int2str(iqid++), "type", "get", 
									"to", childs[j].getAttribute("id") + "@g.us", "xmlns", "w:g"));
							req.addChild(iq);
							gw3++;
							outbuffer = outbuffer + serialize_tree(&req);
						}
					} else if (childs[j].getTag() == "participant") {
						std::string gid = getusername(treelist[i].getAttribute("from"));
						std::string pt = getusername(childs[j].getAttribute("jid"));
						if (groups.find(gid) != groups.end()) {
							groups.find(gid)->second.participants.push_back(pt);
						}
						if (!acc)
							gw3--;
						acc = 1;
					} else if (childs[j].getTag() == "add") {

					}
				}

				t = treelist[i].getChild("group");
				if (t.getTag() != "treeerr") {
					if (t.hasAttributeValue("type", "preview"))
						this->addPreviewPicture(treelist[i].getAttribute("from"), t.getData());
					if (t.hasAttributeValue("type", "image"))
						this->addFullsizePicture(treelist[i].getAttribute("from"), t.getData());
				}
			}
			if (treelist[i].hasAttribute("from") and treelist[i].hasAttribute("id") and 
				treelist[i].hasAttributeValue("xmlns","urn:xmpp:ping")) {
				this->doPong(treelist[i].getAttribute("id"), treelist[i].getAttribute("from"));
			}
		}
	}

	if (gq_stat == 8 and recv_messages_delay.size() != 0) {
		DEBUG_PRINT ("Delayed messages -> Messages");
		for (unsigned int i = 0; i < recv_messages_delay.size(); i++) {
			recv_messages.push_back(recv_messages_delay[i]);
		}
		recv_messages_delay.clear();
	}
}

DataBuffer WhatsappConnection::serialize_tree(Tree * tree, bool crypt)
{
	DEBUG_PRINT( tree->toString() );

	DataBuffer data = write_tree(tree);
	if (data.size() > 65535) {
		std::cerr << "Skipping huge tree! " << data.size() << std::endl;
		return DataBuffer();
	}
	unsigned char flag = 0;
	if (crypt) {
		data = data.encodedBuffer(this->out, &this->session_key[20*1], true, this->frame_seq++);
		flag = 0x80;
	}

	DataBuffer ret;
	ret.putInt(flag, 1);
	ret.putInt(data.size(), 2);
	ret = ret + data;
	return ret;
}

DataBuffer WhatsappConnection::write_tree(Tree * tree)
{
	DataBuffer bout;
	int len = 1;

	if (tree->getAttributes().size() != 0)
		len += tree->getAttributes().size() * 2;
	if (tree->getChildren().size() != 0)
		len++;
	if (tree->getData().size() != 0 or tree->forcedData())
		len++;

	bout.writeListSize(len);
	if (tree->getTag() == "start")
		bout.putInt(1, 1);
	else
		bout.putString(tree->getTag());
	tree->writeAttributes(&bout);

	if (tree->getData().size() > 0 or tree->forcedData())
		bout.putRawString(tree->getData());
	if (tree->getChildren().size() > 0) {
		bout.writeListSize(tree->getChildren().size());

		for (unsigned int i = 0; i < tree->getChildren().size(); i++) {
			DataBuffer tt = write_tree(&tree->getChildren()[i]);
			bout = bout + tt;
		}
	}
	return bout;
}

Tree WhatsappConnection::parse_tree(DataBuffer * data)
{
	int bflag = (data->getInt(1) & 0xF0) >> 4;
	int bsize = data->getInt(2, 1);
	if (bsize > data->size() - 3) {
		return Tree("treeerr");	/* Next message incomplete, return consumed data */
	}
	data->popData(3);

	if (bflag & 8) {
		/* Decode data, buffer conversion */
		if (this->in != NULL) {
			DataBuffer *decoded_data = data->decodedBuffer(this->in, bsize, false);

			Tree tt = read_tree(decoded_data);

			/* Call recursive */
			data->popData(bsize);	/* Pop data unencrypted for next parsing! */
			
			/* Remove hash */
			decoded_data->popData(4); 
			
			delete decoded_data;
			
			return tt;
		} else {
			printf("Received crypted data before establishing crypted layer! Skipping!\n");
			data->popData(bsize);
			return Tree("treeerr");
		}
	} else {
		return read_tree(data);
	}
}

Tree WhatsappConnection::read_tree(DataBuffer * data)
{
	int lsize = data->readListSize();
	int type = data->getInt(1);
	if (type == 1) {
		data->popData(1);
		Tree t;
		t.readAttributes(data, lsize);
		t.setTag("start");
		return t;
	} else if (type == 2) {
		data->popData(1);
		return Tree("treeerr");	/* No data in this tree... */
	}

	Tree t;
	t.setTag(data->readString());
	t.readAttributes(data, lsize);

	if ((lsize & 1) == 1) {
		return t;
	}

	if (data->isList()) {
		t.setChildren(data->readList(this));
	} else {
		t.setData(data->readString());
	}

	return t;
}

static int isgroup(const std::string user)
{
	return (user.find('-') != std::string::npos);
}

void WhatsappConnection::receiveMessage(const Message & m)
{
	/* Push message to user and generate a response */
	Message *mc = m.copy();
	
	if (isgroup(m.from) and gq_stat != 8)	{/* Delay the group message deliver if we do not have the group list */
		recv_messages_delay.push_back(mc);
		DEBUG_PRINT("Received delayed message!");
	}else
		recv_messages.push_back(mc);

	DEBUG_PRINT("Received message type " << m.type() << " from " << m.from << " at " << m.t);

	/* Now add the contact in the list (to query the profile picture) */
	if (contacts.find(m.from) == contacts.end())
		contacts[m.from] = Contact(m.from, false);
	this->addContacts(std::vector < std::string > ());
}

void WhatsappConnection::notifyLastSeen(std::string from, std::string seconds)
{
	from = getusername(from);
	contacts[from].last_seen = str2lng(seconds);
}

void WhatsappConnection::notifyPresence(std::string from, std::string status)
{
	if (status == "")
		status = "available";
	from = getusername(from);
	contacts[from].presence = status;
	user_changes.push_back(from);
}

void WhatsappConnection::addPreviewPicture(std::string from, std::string picture)
{
	from = getusername(from);
	if (contacts.find(from) == contacts.end()) {
		Contact newc(from, false);
		contacts[from] = newc;
	}
	contacts[from].ppprev = picture;
	user_icons.push_back(from);
}

void WhatsappConnection::addFullsizePicture(std::string from, std::string picture)
{
	from = getusername(from);
	if (contacts.find(from) == contacts.end()) {
		Contact newc(from, false);
		contacts[from] = newc;
	}
	contacts[from].pppicture = picture;
}

void WhatsappConnection::setMyPresence(std::string s, std::string msg)
{
	sendRead = (s == "available");
	if (s == "available-noread") {
		s = "available";
	}

	if (s != mypresence) {
		mypresence = s;
		notifyMyPresence();
	}
	if (msg != mymessage) {
		mymessage = msg;
		notifyMyMessage();	/*TODO */
	}
}

void WhatsappConnection::notifyMyPresence()
{
	/* Send the nickname and the current status */
	Tree pres("presence", makeAttr2("name", nickname, "type", mypresence));

	outbuffer = outbuffer + serialize_tree(&pres);
}

void WhatsappConnection::sendInitial()
{
	Tree conf("config");
	Tree iq("iq", makeAttr4("id", int2str(iqid++), "type", "get", "to", whatsappserver, "xmlns", "urn:xmpp:whatsapp:push"));
	iq.addChild(conf);	

	outbuffer = outbuffer + serialize_tree(&iq);
}

void WhatsappConnection::notifyMyMessage()
{
	/* Send the status message */
	Tree xhash("x", makeAttr1("xmlns", "jabber:x:event"));
	xhash.addChild(Tree("server"));
	Tree tbody("body");
	tbody.setData(this->mymessage);

	Tree mes("message", makeAttr3("to", "s.us", "type", "chat", "id", int2str(time(NULL)) + "-" + int2str(iqid++)));
	mes.addChild(xhash);
	mes.addChild(tbody);

	outbuffer = outbuffer + serialize_tree(&mes);
}

void WhatsappConnection::notifyError(ErrorCode err)
{

}

// Returns an integer indicating the next message type (sorting by timestamp)
int WhatsappConnection::query_next() {
	int res = -1;
	unsigned int cur_ts = ~0;
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->t < cur_ts) {
			cur_ts = recv_messages[i]->t;
			res = recv_messages[i]->type();
		}
	}
	return res;
}

bool WhatsappConnection::query_chat(std::string & from, std::string & message, std::string & author, unsigned long &t)
{
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->type() == 0) {
			from = recv_messages[i]->from;
			t = recv_messages[i]->t;
			message = ((ChatMessage *) recv_messages[i])->message;
			author = ((ChatMessage *) recv_messages[i])->author;
			delete recv_messages[i];
			recv_messages.erase(recv_messages.begin() + i);
			return true;
		}
	}
	return false;
}

bool WhatsappConnection::query_chatimages(std::string & from, std::string & preview, std::string & url, std::string & author, unsigned long &t)
{
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->type() == 1) {
			from = recv_messages[i]->from;
			t = recv_messages[i]->t;
			preview = ((ImageMessage *) recv_messages[i])->preview;
			url = ((ImageMessage *) recv_messages[i])->url;
			author = ((ImageMessage *) recv_messages[i])->author;
			delete recv_messages[i];
			recv_messages.erase(recv_messages.begin() + i);
			return true;
		}
	}
	return false;
}

bool WhatsappConnection::query_chatsounds(std::string & from, std::string & url, std::string & author, unsigned long &t)
{
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->type() == 3) {
			from = recv_messages[i]->from;
			t = recv_messages[i]->t;
			url = ((SoundMessage *) recv_messages[i])->url;
			author = ((SoundMessage *) recv_messages[i])->author;
			delete recv_messages[i];
			recv_messages.erase(recv_messages.begin() + i);
			return true;
		}
	}
	return false;
}

bool WhatsappConnection::query_chatvideos(std::string & from, std::string & url, std::string & author, unsigned long &t)
{
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->type() == 4) {
			from = recv_messages[i]->from;
			t = recv_messages[i]->t;
			url = ((VideoMessage *) recv_messages[i])->url;
			author = ((VideoMessage *) recv_messages[i])->author;
			delete recv_messages[i];
			recv_messages.erase(recv_messages.begin() + i);
			return true;
		}
	}
	return false;
}

bool WhatsappConnection::query_chatlocations(std::string & from, double &lat, double &lng, std::string & prev, std::string & author, unsigned long &t)
{
	for (unsigned int i = 0; i < recv_messages.size(); i++) {
		if (recv_messages[i]->type() == 2) {
			from = recv_messages[i]->from;
			t = recv_messages[i]->t;
			prev = ((LocationMessage *) recv_messages[i])->preview;
			lat = ((LocationMessage *) recv_messages[i])->latitude;
			lng = ((LocationMessage *) recv_messages[i])->longitude;
			author = ((LocationMessage *) recv_messages[i])->author;
			delete recv_messages[i];
			recv_messages.erase(recv_messages.begin() + i);
			return true;
		}
	}
	return false;
}

int WhatsappConnection::getuserstatus(const std::string & who)
{
	if (contacts.find(who) != contacts.end()) {
		if (contacts[who].presence == "available")
			return 1;
		return 0;
	}
	return -1;
}

std::string WhatsappConnection::getuserstatusstring(const std::string & who)
{
	if (contacts.find(who) != contacts.end()) {
		return contacts[who].status;
	}
	return "";
}

unsigned long long WhatsappConnection::getlastseen(const std::string & who)
{
	/* Schedule a last seen update, just in case */
	this->getLast(std::string(who) + "@" + whatsappserver);

	if (contacts.find(who) != contacts.end()) {
		return contacts[who].last_seen;
	}
	return ~0;
}

bool WhatsappConnection::query_status(std::string & from, int &status)
{
	while (user_changes.size() > 0) {
		if (contacts.find(user_changes[0]) != contacts.end()) {
			from = user_changes[0];
			status = 0;
			if (contacts[from].presence == "available")
				status = 1;

			user_changes.erase(user_changes.begin());
			return true;
		}
		user_changes.erase(user_changes.begin());
	}
	return false;
}

bool WhatsappConnection::query_typing(std::string & from, int &status)
{
	while (user_typing.size() > 0) {
		if (contacts.find(user_typing[0]) != contacts.end()) {
			from = user_typing[0];
			status = 0;
			if (contacts[from].typing == "composing")
				status = 1;

			user_typing.erase(user_typing.begin());
			return true;
		}
		user_typing.erase(user_typing.begin());
	}
	return false;
}

bool WhatsappConnection::query_icon(std::string & from, std::string & icon, std::string & hash)
{
	while (user_icons.size() > 0) {
		if (contacts.find(user_icons[0]) != contacts.end()) {
			from = user_icons[0];
			icon = contacts[from].ppprev;
			hash = "";

			user_icons.erase(user_icons.begin());
			return true;
		}
		user_icons.erase(user_icons.begin());
	}
	return false;
}

bool WhatsappConnection::query_avatar(std::string user, std::string & icon)
{
	user = getusername(user);
	if (contacts.find(user) != contacts.end()) {
		icon = contacts[user].pppicture;
		if (icon.size() == 0) {
			/* Return preview icon and query the fullsize picture */
			/* for future displays to save bandwidth */
			this->queryFullSize(user + "@" + whatsappserver);
			icon = contacts[user].ppprev;
		}
		return true;
	}
	return false;
}

void WhatsappConnection::doPong(std::string id, std::string from)
{
	std::map < std::string, std::string > auth;
	auth["to"] = from;
	auth["id"] = id;
	auth["type"] = "result";
	Tree t("iq", auth);

	outbuffer = outbuffer + serialize_tree(&t);
}

void WhatsappConnection::sendResponse()
{
	std::map < std::string, std::string > auth;
	Tree t("response", auth);

	std::string response = phone + challenge_data + int2str(time(NULL));
	DataBuffer eresponse(response.c_str(), response.size());
	eresponse = eresponse.encodedBuffer(this->out, &this->session_key[20*1], false, this->frame_seq++);
	response = eresponse.toString();
	t.setData(response);

	outbuffer = outbuffer + serialize_tree(&t, false);
}


class WhatsappConnectionAPI {
private:
	WhatsappConnection * connection;

public:
	WhatsappConnectionAPI(std::string phone, std::string password, std::string nick);
	~WhatsappConnectionAPI();

	void doLogin(std::string);
	void receiveCallback(const char *data, int len);
	int sendCallback(char *data, int len);
	void sentCallback(int len);
	bool hasDataToSend();

	bool queryReceivedMessage(char *msgid, int * type);
	void getMessageId(char * msgid);
	void addContacts(std::vector < std::string > clist);
	void sendChat(std::string msgid, std::string to, std::string message);
	void sendGroupChat(std::string msgid, std::string to, std::string message);
	bool query_chat(std::string & from, std::string & message, std::string & author, unsigned long &t);
	bool query_chatimages(std::string & from, std::string & preview, std::string & url, std::string & author, unsigned long &t);
	bool query_chatsounds(std::string & from, std::string & url, std::string & author, unsigned long &t);
	bool query_chatvideos(std::string & from, std::string & url, std::string & author, unsigned long &t);
	bool query_chatlocations(std::string & from, double &lat, double &lng, std::string & prev, std::string & author, unsigned long &t);
	bool query_status(std::string & from, int &status);
	int query_next();
	bool query_icon(std::string & from, std::string & icon, std::string & hash);
	bool query_avatar(std::string user, std::string & icon);
	bool query_typing(std::string & from, int &status);
	void account_info(unsigned long long &creation, unsigned long long &freeexp, std::string & status);
	void send_avatar(const std::string & avatar);
	int getuserstatus(const std::string & who);
	std::string getuserstatusstring(const std::string & who);
	unsigned long long getlastseen(const std::string & who);
	void addGroup(std::string subject);
	void leaveGroup(std::string group);
	void manageParticipant(std::string group, std::string participant, std::string command);

	void notifyTyping(std::string who, int status);
	void setMyPresence(std::string s, std::string msg);

	std::map < std::string, Group > getGroups();
	bool groupsUpdated();

	int loginStatus() const;

	int sendImage(std::string to, int w, int h, unsigned int size, const char *fp);

	int sendSSLCallback(char *buffer, int maxbytes);
	int sentSSLCallback(int bytessent);
	void receiveSSLCallback(char *buffer, int bytesrecv);
	bool hasSSLDataToSend();
	bool closeSSLConnection();
	void SSLCloseCallback();
	bool hasSSLConnection(std::string & host, int *port);
	int uploadProgress(int &rid, int &bs);
	int uploadComplete(int);
};

WhatsappConnectionAPI::WhatsappConnectionAPI(std::string phone, std::string password, std::string nick)
{
	connection = new WhatsappConnection(phone, password, nick);
}

WhatsappConnectionAPI::~WhatsappConnectionAPI()
{
	delete connection;
}

std::map < std::string, Group > WhatsappConnectionAPI::getGroups()
{
	return connection->getGroups();
}

bool WhatsappConnectionAPI::groupsUpdated()
{
	return connection->groupsUpdated();
}

int WhatsappConnectionAPI::getuserstatus(const std::string & who)
{
	return connection->getuserstatus(who);
}

void WhatsappConnectionAPI::addGroup(std::string subject)
{
	connection->addGroup(subject);
}

void WhatsappConnectionAPI::leaveGroup(std::string subject)
{
	connection->leaveGroup(subject);
}

void WhatsappConnectionAPI::manageParticipant(std::string group, std::string participant, std::string command)
{
	connection->manageParticipant(group, participant, command);
}

unsigned long long WhatsappConnectionAPI::getlastseen(const std::string & who)
{
	return connection->getlastseen(who);
}

int WhatsappConnectionAPI::query_next() {
	return connection->query_next();
}

int WhatsappConnectionAPI::sendImage(std::string to, int w, int h, unsigned int size, const char *fp)
{
	return connection->sendImage(to, w, h, size, fp);
}

int WhatsappConnectionAPI::uploadProgress(int &rid, int &bs)
{
	return connection->uploadProgress(rid, bs);
}
int WhatsappConnectionAPI::uploadComplete(int rid)
{
	return connection->uploadComplete(rid);
}

void WhatsappConnectionAPI::send_avatar(const std::string & avatar)
{
	connection->send_avatar(avatar);
}

bool WhatsappConnectionAPI::query_icon(std::string & from, std::string & icon, std::string & hash)
{
	return connection->query_icon(from, icon, hash);
}

bool WhatsappConnectionAPI::query_avatar(std::string user, std::string & icon)
{
	return connection->query_avatar(user, icon);
}

bool WhatsappConnectionAPI::query_typing(std::string & from, int &status)
{
	return connection->query_typing(from, status);
}

void WhatsappConnectionAPI::setMyPresence(std::string s, std::string msg)
{
	connection->setMyPresence(s, msg);
}

void WhatsappConnectionAPI::notifyTyping(std::string who, int status)
{
	connection->notifyTyping(who, status);
}

std::string WhatsappConnectionAPI::getuserstatusstring(const std::string & who)
{
	return connection->getuserstatusstring(who);
}

bool WhatsappConnectionAPI::query_chatimages(std::string & from, std::string & preview, std::string & url, std::string & author, unsigned long &t)
{
	return connection->query_chatimages(from, preview, url, author, t);
}

bool WhatsappConnectionAPI::query_chatsounds(std::string & from, std::string & url, std::string & author, unsigned long &t)
{
	return connection->query_chatsounds(from, url, author, t);
}
bool WhatsappConnectionAPI::query_chatvideos(std::string & from, std::string & url, std::string & author, unsigned long &t)
{
	return connection->query_chatvideos(from, url, author, t);
}

bool WhatsappConnectionAPI::query_chat(std::string & from, std::string & msg, std::string & author, unsigned long &t)
{
	return connection->query_chat(from, msg, author, t);
}

bool WhatsappConnectionAPI::query_chatlocations(std::string & from, double &lat, double &lng, std::string & prev, std::string & author, unsigned long &t)
{
	return connection->query_chatlocations(from, lat, lng, prev, author, t);
}

bool WhatsappConnectionAPI::query_status(std::string & from, int &status)
{
	return connection->query_status(from, status);
}

void WhatsappConnectionAPI::getMessageId(char * msgid)
{
	connection->getMessageId(msgid);
}

bool WhatsappConnectionAPI::queryReceivedMessage(char * msgid, int * type)
{
	return connection->queryReceivedMessage(msgid, type);
}

void WhatsappConnectionAPI::sendChat(std::string msgid, std::string to, std::string message)
{
	connection->sendChat(msgid, to, message);
}

void WhatsappConnectionAPI::sendGroupChat(std::string msgid, std::string to, std::string message)
{
	connection->sendGroupChat(msgid, to, message);
}

int WhatsappConnectionAPI::loginStatus() const
{
	return connection->loginStatus();
}

void WhatsappConnectionAPI::doLogin(std::string resource)
{
	connection->doLogin(resource);
}

void WhatsappConnectionAPI::receiveCallback(const char *data, int len)
{
	connection->receiveCallback(data, len);
}

int WhatsappConnectionAPI::sendCallback(char *data, int len)
{
	return connection->sendCallback(data, len);
}

void WhatsappConnectionAPI::sentCallback(int len)
{
	connection->sentCallback(len);
}

void WhatsappConnectionAPI::addContacts(std::vector < std::string > clist)
{
	connection->addContacts(clist);
}

bool WhatsappConnectionAPI::hasDataToSend()
{
	return connection->hasDataToSend();
}

void WhatsappConnectionAPI::account_info(unsigned long long &creation, unsigned long long &freeexp, std::string & status)
{
	connection->account_info(creation, freeexp, status);
}

int WhatsappConnectionAPI::sendSSLCallback(char *buffer, int maxbytes)
{
	return connection->sendSSLCallback(buffer, maxbytes);
}

int WhatsappConnectionAPI::sentSSLCallback(int bytessent)
{
	return connection->sentSSLCallback(bytessent);
}

void WhatsappConnectionAPI::receiveSSLCallback(char *buffer, int bytesrecv)
{
	connection->receiveSSLCallback(buffer, bytesrecv);
}

bool WhatsappConnectionAPI::hasSSLDataToSend()
{
	return connection->hasSSLDataToSend();
}

bool WhatsappConnectionAPI::closeSSLConnection()
{
	return connection->closeSSLConnection();
}

void WhatsappConnectionAPI::SSLCloseCallback()
{
	connection->SSLCloseCallback();
}

bool WhatsappConnectionAPI::hasSSLConnection(std::string & host, int *port)
{
	return connection->hasSSLConnection(host, port);
}


