/**
 * management.cpp
 * This file is part of the YATE Project http://YATE.null.ro 
 *
 * Yet Another Signalling Stack - implements the support for SS7, ISDN and PSTN
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "yatesig.h"
#include <yatephone.h>

#include <string.h>

using namespace TelEngine;

#define MAKE_NAME(x) { #x, SS7MsgSNM::x }
static const TokenDict s_snm_names[] = {
    // this list must be kept in synch with the header
    MAKE_NAME(COO),
    MAKE_NAME(ECO),
    MAKE_NAME(RCT),
    MAKE_NAME(TFP),
    MAKE_NAME(RST),
    MAKE_NAME(RSP), // alias
    MAKE_NAME(LIN),
    MAKE_NAME(TRA),
    MAKE_NAME(DLC),
    MAKE_NAME(UPU),
    MAKE_NAME(COA),
    MAKE_NAME(ECA),
    MAKE_NAME(TFC),
    MAKE_NAME(TCP),
    MAKE_NAME(TFPA), // alias
    MAKE_NAME(RSR),
    MAKE_NAME(LUN),
    MAKE_NAME(TRW),
    MAKE_NAME(CSS),
    MAKE_NAME(XCO),
    MAKE_NAME(TFR),
    MAKE_NAME(RCP),
    MAKE_NAME(LIA),
    MAKE_NAME(CNS),
    MAKE_NAME(XCA),
    MAKE_NAME(TCR),
    MAKE_NAME(RCR),
    MAKE_NAME(LUA),
    MAKE_NAME(CNP),
    MAKE_NAME(CBD),
    MAKE_NAME(TFA),
    MAKE_NAME(LID),
    MAKE_NAME(CBA),
    MAKE_NAME(TCA),
    MAKE_NAME(TFAA), // alias
    MAKE_NAME(LFU),
    MAKE_NAME(LLT),
    MAKE_NAME(LLI), // alias
    MAKE_NAME(LRT),
    MAKE_NAME(LRI), // alias
    { 0, 0 }
};

static const TokenDict s_snm_group[] = {
    // this list must be kept in synch with the header
    MAKE_NAME(CHM),
    MAKE_NAME(ECM),
    MAKE_NAME(FCM),
    MAKE_NAME(TFM),
    MAKE_NAME(RSM),
    MAKE_NAME(MIM),
    MAKE_NAME(TRM),
    MAKE_NAME(DLM),
    MAKE_NAME(UFC),
    { 0, 0 }
};
#undef MAKE_NAME

namespace { //anonymous

class SnmPending : public SignallingMessageTimer, public SS7Label
{
public:
    inline SnmPending(SS7MSU* msg, const SS7Label& label, int txSls, u_int64_t interval, u_int64_t global = 0)
	: SignallingMessageTimer(interval,global), SS7Label(label),
	  m_msu(msg), m_txSls(txSls)
	{ }

    inline ~SnmPending()
	{ TelEngine::destruct(m_msu); }

    inline SS7MSU& msu() const
	{ return *m_msu; }

    inline int txSls() const
	{ return m_txSls; }

    inline SS7MsgSNM::Type snmType() const
	{ return (SS7MsgSNM::Type)m_msu->at(length()+1,0); }

    inline const char* snmName() const
	{ return SS7MsgSNM::lookup(snmType(),"Unknown"); }

    inline bool matches(const SS7Label& lbl) const
	{ return opc() == lbl.dpc() && dpc() == lbl.opc() && sls() == lbl.sls(); }

private:
    SS7MSU* m_msu;
    int m_txSls;
};

}; // anonymous namespace

// Constructor
SS7MsgSNM::SS7MsgSNM(unsigned char type)
    : SignallingMessage(lookup((Type)type,"Unknown")),
    m_type(type)
{
}

void SS7MsgSNM::toString(String& dest, const SS7Label& label, bool params) const
{
    const char* enclose = "\r\n-----";
    dest = enclose;
    dest << "\r\n" << name() << " [label=" << label << ']';
    if (params) {
	unsigned int n = m_params.length();
	for (unsigned int i = 0; i < n; i++) {
	    NamedString* s = m_params.getParam(i);
	    if (s)
		dest << "\r\n  " << s->name() << "='" << *s << "'";
	}
    }
    dest << enclose;
}

// Parse a received buffer and build a message from it
SS7MsgSNM* SS7MsgSNM::parse(SS7Management* receiver, unsigned char type,
    SS7PointCode::Type pcType, const unsigned char* buf, unsigned int len)
{
    const char* pct = SS7PointCode::lookup(pcType);
    if (!pct)
	return 0;
    SS7MsgSNM* msg = new SS7MsgSNM(type);
    msg->params().addParam("pointcodetype",pct);
#ifdef XDEBUG
    String tmp;
    tmp.hexify((void*)buf,len,' ');
    Debug(receiver,DebugAll,"Decoding msg=%s pctype=%s buf: %s [%p]",
	msg->name(),pct,tmp.c_str(),receiver);
#endif
    // TODO: parse the rest of the message. Check extra bytes (message specific)
    if (!(buf && len))
	return msg;
    do {
	// TFP,TFR,TFA: Q.704 15.7 The must be at lease 2 bytes in buffer
	if (type == TFP || type == TFR || type == TFA) {
	    // 2 bytes destination
	    SS7PointCode pc;
	    unsigned char spare;
	    if (pc.assign(pcType,buf,len,&spare)) {
		String tmp;
		tmp << pc;
		msg->params().addParam("destination",tmp);
		if (spare) {
		    tmp.hexify(&spare,1);
		    msg->params().addParam("spare",tmp);
		}
	    }
	    else
		Debug(receiver,DebugNote,
		    "Failed to decode destination for msg=%s len=%u [%p]",
		    msg->name(),len,receiver);
	    break;
	}
	// COO,COA: changeover sequence, slc
	else if (type == COO || type == COA) {
	    int seq = -1;
	    int slc = -1;
	    switch (pcType) {
		case SS7PointCode::ITU:
		    if (len >= 1)
			seq = buf[0];
		    break;
		case SS7PointCode::ANSI:
		    if (len >= 2) {
			slc = buf[0] & 0x0f;
			seq = (buf[0] >> 4) | (((unsigned int)buf[1]) << 4);
		    }
		    break;
		default:
		    Debug(DebugStub,"Please implement COO decoding for type %u",pcType);
	    }
	    if (seq >= 0)
		msg->params().addParam("sequence",String(seq));
	    if (slc >= 0)
		msg->params().addParam("slc",String(slc));
	}
	// CBD,CBA: changeback code, slc
	else if (type == CBD || type == CBA) {
	    int code = -1;
	    int slc = -1;
	    switch (pcType) {
		case SS7PointCode::ITU:
		    if (len >= 1)
			code = buf[0];
		    break;
		case SS7PointCode::ANSI:
		    if (len >= 2) {
			slc = buf[0] & 0x0f;
			code = (buf[0] >> 4) | (((unsigned int)buf[1]) << 4);
		    }
		    break;
		default:
		    Debug(DebugStub,"Please implement CBD decoding for type %u",pcType);
	    }
	    if (code >= 0)
		msg->params().addParam("code",String(code));
	    if (slc >= 0)
		msg->params().addParam("slc",String(slc));
	}
	// UPU: user part ID, unavailability cause
	else if (type == UPU) {
	    SS7PointCode pc;
	    unsigned char spare;
	    if (pc.assign(pcType,buf,len,&spare)) {
		String tmp;
		tmp << pc;
		msg->params().addParam("destination",tmp);
		if (spare) {
		    tmp.hexify(&spare,1);
		    msg->params().addParam("spare",tmp);
		}
		unsigned int dlen = SS7PointCode::length(pcType);
		if (dlen < len) {
		    msg->params().addParam("part",String((unsigned int)buf[dlen] & 0x0f));
		    msg->params().addParam("cause",String((unsigned int)buf[dlen] >> 4));
		}
	    }
	    else
		Debug(receiver,DebugNote,
		    "Failed to decode destination for msg=%s len=%u [%p]",
		    msg->name(),len,receiver);
	}
    } while (false);
    return msg;
}

const TokenDict* SS7MsgSNM::names()
{
    return s_snm_names;
}


#define MAKE_NAME(x) { #x, SS7MsgMTN::x }
static const TokenDict s_mtn_names[] = {
    // this list must be kept in synch with the header
    MAKE_NAME(SLTM),
    MAKE_NAME(SLTA),
    { 0, 0 }
};
#undef MAKE_NAME

const TokenDict* SS7MsgMTN::names()
{
    return s_mtn_names;
}


// Control operations
static const TokenDict s_dict_control[] = {
    { "prohibit", SS7MsgSNM::TFP },
    { "restrict", SS7MsgSNM::TFR },
    { "congest", SS7MsgSNM::TFC },
    { "allow", SS7MsgSNM::TFA },
    { "restart", SS7MsgSNM::TRA },
    { "changeover", SS7MsgSNM::COO },
    { "changeback", SS7MsgSNM::CBD },
    { "link-inhibit", SS7MsgSNM::LIN },
    { "link-uninhibit", SS7MsgSNM::LUN },
    { "link-force-uninhibit", SS7MsgSNM::LFU },
    { "test-congestion", SS7MsgSNM::RCT },
    { "test-prohibited", SS7MsgSNM::RST },
    { "test-restricted", SS7MsgSNM::RSR },
    { 0, 0 }
};

HandledMSU SS7Management::receivedMSU(const SS7MSU& msu, const SS7Label& label, SS7Layer3* network, int sls)
{
    if (msu.getSIF() != sif())
	return HandledMSU::Rejected;
    if (network) {
	unsigned int local = network->getLocal(label.type());
	if (local && label.dpc().pack(label.type()) != local)
	    return HandledMSU::Rejected;
    }
    SS7Router* router = YOBJECT(SS7Router,SS7Layer4::network());
    if (router && (router != network)) {
	unsigned int local = network->getLocal(label.type());
	if (local && label.dpc().pack(label.type()) != local)
	    return HandledMSU::Rejected;
    }

    unsigned int len = msu.length() - label.length() - 1;
    // according to Q.704 there should be at least the heading codes (8 bit)
    const unsigned char* buf = msu.getData(label.length()+1,1);
    if (!buf)
	return false;
    SS7MsgSNM* msg = SS7MsgSNM::parse(this,buf[0],label.type(),buf+1,len-1);
    if (!msg)
	return false;

    if (debugAt(DebugInfo)) {
	String tmp;
	msg->toString(tmp,label,debugAt(DebugAll));
	Debug(this,DebugInfo,"Received %u bytes message (%p) on %d%s",
	    len,msg,sls,tmp.c_str());
    }

    String addr;
    addr << label;
    if (msg->type() == SS7MsgSNM::TFP ||
	msg->type() == SS7MsgSNM::TFR ||
	msg->type() == SS7MsgSNM::TFA) {
	String dest = msg->params().getValue("destination");
	if (!dest.null()) {
	    if (debugAt(DebugInfo)) {
		const char* status = (msg->type() == SS7MsgSNM::TFP) ? "prohibited" :
		    ((msg->type() == SS7MsgSNM::TFA) ? "allowed" : "restricted");
		Debug(this,DebugInfo,"%s (label=%s): Traffic is %s to dest=%s [%p]",
		    msg->name(),addr.c_str(),status,dest.c_str(),this);
	    }
	    if (router) {
		NamedList* ctrl = router->controlCreate();
		if (ctrl) {
		    ctrl->copyParams(msg->params());
		    switch (msg->type()) {
			case SS7MsgSNM::TFP:
			    ctrl->setParam("operation","prohibit");
			    break;
			case SS7MsgSNM::TFR:
			    ctrl->setParam("operation","restrict");
			    break;
			case SS7MsgSNM::TFA:
			    ctrl->setParam("operation","allow");
			    break;
		    }
		    ctrl->setParam("automatic",String::boolText(true));
		    router->controlExecute(ctrl);
		}
	    }
	}
	else
	    Debug(this,DebugNote,"Received %s (label=%s) without destination [%p]",
		    msg->name(),addr.c_str(),this);
    }
    else if (msg->type() == SS7MsgSNM::TRA) {
	String dest;
	dest << label.opc();
	Debug(this,DebugInfo,"%s (label=%s): Traffic can restart to dest=%s [%p]",
	    msg->name(),addr.c_str(),dest.c_str(),this);
	if (router) {
	    NamedList* ctrl = router->controlCreate("allowed");
	    if (ctrl) {
		ctrl->copyParams(msg->params());
		ctrl->setParam("destination",dest);
		ctrl->setParam("automatic",String::boolText(true));
		router->controlExecute(ctrl);
	    }
	}
    }
    else if (msg->type() == SS7MsgSNM::COO ||
	msg->type() == SS7MsgSNM::XCO ||
	msg->type() == SS7MsgSNM::ECO) {
	if (!len--)
	    return false;
	const unsigned char* s = msu.getData(label.length()+2,len);
	if (!s)
	    return false;
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	SS7Label lbl(label,label.sls(),0);
	if (inhibit(lbl,SS7Layer2::Inactive)) {
	    String link;
	    link << msg->params().getValue("pointcodetype") << "," << lbl;
	    Debug(this,DebugNote,"Changeover order on %s",link.c_str());
	    int seq = msg->params().getIntValue("sequence",-1);
	    if (seq >= 0)
		recover(lbl,seq);
	    seq = router ? router->getSequence(lbl) : -1;
	    if (seq >= 0) {
		int len = 2;
		unsigned char data[3];
		data[0] = SS7MsgSNM::COA;
		switch (label.type()) {
		    case SS7PointCode::ITU:
			data[1] = (unsigned char)seq;
			break;
		    case SS7PointCode::ANSI:
			data[1] = (unsigned char)((msg->params().getIntValue("slc",sls) & 0x0f) | (seq << 4));
			data[2] = (unsigned char)(seq >> 4);
			len = 3;
			break;
		    default:
			Debug(DebugStub,"Please implement COO for type %u",label.type());
			return false;
		}
		return transmitMSU(SS7MSU(msu.getSIO(),lbl,&data,len),lbl,sls) >= 0;
	    }
	    else {
		// postpone an ECA in case we are unable to send a COA/XCA
		static unsigned char data = SS7MsgSNM::ECA;
		return postpone(new SS7MSU(msu.getSIO(),lbl,&data,1),lbl,sls,0,200);
	    }
	}
	else
	    Debug(this,DebugMild,"Unexpected %s %s [%p]",msg->name(),addr.c_str(),this);
    }
    else if (msg->type() == SS7MsgSNM::COA ||
	msg->type() == SS7MsgSNM::XCA ||
	msg->type() == SS7MsgSNM::ECA) {
	if (!len--)
	    return false;
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	lock();
	SnmPending* pend = 0;
	for (ObjList* l = m_pending.skipNull(); l; l = l->skipNext()) {
	    SnmPending* p = static_cast<SnmPending*>(l->get());
	    const unsigned char* ptr = p->msu().getData(p->length()+1,len+1);
	    if (!(ptr && p->matches(label)))
		continue;
	    switch (ptr[0]) {
		case SS7MsgSNM::COO:
		case SS7MsgSNM::XCO:
		case SS7MsgSNM::ECO:
		    break;
		default:
		    continue;
	    }
	    pend = static_cast<SnmPending*>(m_pending.remove(p,false));
	    break;
	}
	unlock();
	if (pend) {
	    String link;
	    link << msg->params().getValue("pointcodetype") << "," << *pend;
	    Debug(this,DebugNote,"Changeover acknowledged on %s",link.c_str());
	    inhibit(*pend,SS7Layer2::Inactive);
	    int seq = msg->params().getIntValue("sequence",-1);
	    if (seq >= 0)
		recover(*pend,seq);
	}
	else
	    Debug(this,DebugMild,"Unexpected %s %s [%p]",msg->name(),addr.c_str(),this);
	TelEngine::destruct(pend);
    }
    else if (msg->type() == SS7MsgSNM::CBD) {
	if (!len--)
	    return false;
	const unsigned char* s = msu.getData(label.length()+2,len);
	if (!s)
	    return false;
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	SS7Label lbl(label,label.sls(),0);
	if (inhibit(lbl,0,SS7Layer2::Inactive)) {
	    String link;
	    link << msg->params().getValue("pointcodetype") << "," << lbl;
	    Debug(this,DebugNote,"Changeback declaration on %s",link.c_str());
	    SS7MSU answer(msu.getSIO(),lbl,0,len+1);
	    unsigned char* d = answer.getData(lbl.length()+1,len+1);
	    if (!d)
		return false;
	    *d++ = SS7MsgSNM::CBA;
	    while (len--)
		*d++ = *s++;
	    return transmitMSU(answer,lbl,sls) >= 0;
	}
	else
	    Debug(this,DebugMild,"Unexpected %s %s [%p]",msg->name(),addr.c_str(),this);
    }
    else if (msg->type() == SS7MsgSNM::CBA) {
	if (!len--)
	    return false;
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	lock();
	SnmPending* pend = 0;
	for (ObjList* l = m_pending.skipNull(); l; l = l->skipNext()) {
	    SnmPending* p = static_cast<SnmPending*>(l->get());
	    if (p->msu().length() != msu.length())
		continue;
	    const unsigned char* ptr = p->msu().getData(p->length()+1,len+1);
	    if (!ptr || (ptr[0] != SS7MsgSNM::CBD))
		continue;
	    if (::memcmp(ptr+1,buf+1,len) || !p->matches(label))
		continue;
	    pend = static_cast<SnmPending*>(m_pending.remove(p,false));
	    break;
	}
	unlock();
	if (pend) {
	    String link;
	    link << msg->params().getValue("pointcodetype") << "," << *pend;
	    Debug(this,DebugNote,"Changeback acknowledged on %s",link.c_str());
	    inhibit(*pend,0,SS7Layer2::Inactive);
	}
	else
	    Debug(this,DebugMild,"Unexpected %s %s [%p]",msg->name(),addr.c_str(),this);
	TelEngine::destruct(pend);
    }
    else if (msg->type() == SS7MsgSNM::LIN) {
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	SS7Label lbl(label,label.sls(),0);
	if (router) {
	    unsigned char data = (router->inhibit(lbl,SS7Layer2::Remote,0,true))
		? SS7MsgSNM::LIA : SS7MsgSNM::LID;
	    return transmitMSU(SS7MSU(msu.getSIO(),lbl,&data,1),lbl,sls) >= 0;
	}
    }
    else if (msg->type() == SS7MsgSNM::LUN || msg->type() == SS7MsgSNM::LFU) {
	Debug(this,DebugAll,"%s (code len=%u) [%p]",msg->name(),len,this);
	SS7Label lbl(label,label.sls(),0);
	if (router && router->inhibit(lbl,0,SS7Layer2::Remote)) {
	    static unsigned char lua = SS7MsgSNM::LUA;
	    return transmitMSU(SS7MSU(msu.getSIO(),lbl,&lua,1),lbl,sls) >= 0;
	}
    }
    else if (msg->type() == SS7MsgSNM::UPU) {
	Debug(this,DebugNote,"Unavailable part %s at %s, cause %s",
	    msg->params().getValue("part","?"),
	    msg->params().getValue("destination","?"),
	    msg->params().getValue("cause","?"));
	if (router) {
	    unsigned char part = msg->params().getIntValue("part",-1);
	    unsigned char cause = msg->params().getIntValue("cause",-1);
	    SS7PointCode pc;
	    if (part > SS7MSU::MTNS && part <= 0x0f && cause <= 0x0f &&
		pc.assign(msg->params().getValue("destination"),label.type()))
		router->receivedUPU(label.type(),pc,(SS7MSU::Services)part,
		    cause,label,sls);
	}
    }
    else {
	String tmp;
	tmp.hexify((void*)buf,len,' ');
	String params;
	unsigned int n = msg->params().count();
	if (n)
	    for (unsigned int i = 0; i < n; i++) {
		NamedString* ns = static_cast<NamedString*>(msg->params().getParam(i));
		if (ns)
		    params.append(String(ns->name()) + "=" + *ns,",");
	    }
	Debug(this,DebugMild,
	    "Unhandled SNM type=%s group=%s label=%s params:%s len=%u: %s ",
	    msg->name(),lookup(msg->group(),s_snm_group,"Spare"),
	    addr.c_str(),params.c_str(),len,tmp.c_str());
    }

    TelEngine::destruct(msg);
    return true;
}

bool SS7Management::control(NamedList& params)
{
    String* ret = params.getParam("completion");
    const String* oper = params.getParam("operation");
    const char* cmp = params.getValue("component");
    int cmd = -1;
    if (!TelEngine::null(oper)) {
	cmd = oper->toInteger(s_dict_control,cmd);
	if (cmd < 0)
	    cmd = oper->toInteger(s_snm_names,cmd);
    }

    if (ret) {
	if (oper && (cmd < 0))
	    return false;
	String part = params.getValue("partword");
	if (cmp) {
	    if (toString() != cmp)
		return false;
	    for (const TokenDict* d = s_dict_control; d->token; d++)
		Module::itemComplete(*ret,d->token,part);
	    return true;
	}
	return Module::itemComplete(*ret,toString(),part);
    }

    if (!(cmp && toString() == cmp))
	return false;

    const String* addr = params.getParam("address");
    if (cmd < 0 || TelEngine::null(addr))
	return SignallingComponent::control(params);
    // TYPE,opc,dpc,sls,spare
    SS7PointCode::Type t = SS7PointCode::Other;
    ObjList* l = addr->split(',');
    if (l->at(0) && (t = SS7PointCode::lookup(l->at(0)->toString())) != SS7PointCode::Other) {
	unsigned char netInd = ni();
	if (network())
	    netInd = network()->getNI(t,netInd);
	unsigned char txSio = getSIO(params,ssf(),prio(),netInd);
	SS7PointCode opc,dpc;
	int sls = -1;
	int spare = 0;
	if (l->at(1) && opc.assign(l->at(1)->toString(),t) &&
	    l->at(2) && dpc.assign(l->at(2)->toString(),t)) {
	    if (l->at(3))
		sls = l->at(3)->toString().toInteger(sls);
	    if (l->at(4))
		spare = l->at(4)->toString().toInteger(spare);
	    TelEngine::destruct(l);
	    SS7Label lbl(t,dpc,opc,sls,spare);
	    int txSls = sls;
	    switch (cmd) {
		case SS7MsgSNM::COO:
		case SS7MsgSNM::COA:
		case SS7MsgSNM::CBD:
		case SS7MsgSNM::CBA:
		    txSls = (txSls + 1) & 0xff;
	    }
	    txSls = params.getIntValue("linksel",txSls);
	    switch (cmd) {
		// Messages containing a destination point code
		case SS7MsgSNM::TFP:
		case SS7MsgSNM::TFA:
		case SS7MsgSNM::TFR:
		case SS7MsgSNM::TFC:
		case SS7MsgSNM::RST:
		case SS7MsgSNM::RSR:
		    {
			addr = params.getParam("destination");
			SS7PointCode dest(opc);
			if (TelEngine::null(addr) || dest.assign(*addr,t)) {
			    unsigned char data[5];
			    data[0] = cmd;
			    return dest.store(t,data+1,spare) &&
				(transmitMSU(SS7MSU(txSio,lbl,data,
				    SS7PointCode::length(t)+1),lbl,txSls) >= 0);
			}
		    }
		    return false;
		// Messages with just the code
		case SS7MsgSNM::ECO:
		case SS7MsgSNM::TRA:
		case SS7MsgSNM::LIN:
		case SS7MsgSNM::LUN:
		case SS7MsgSNM::LIA:
		case SS7MsgSNM::LUA:
		case SS7MsgSNM::LID:
		case SS7MsgSNM::LFU:
		case SS7MsgSNM::LLT:
		case SS7MsgSNM::LRT:
		case SS7MsgSNM::RCT:
		case SS7MsgSNM::CSS:
		case SS7MsgSNM::CNS:
		case SS7MsgSNM::CNP:
		    {
			unsigned char data = cmd;
			return transmitMSU(SS7MSU(txSio,lbl,&data,1),lbl,txSls) >= 0;
		    }
		// Changeover messages
		case SS7MsgSNM::COO:
		case SS7MsgSNM::COA:
		    if (params.getBoolValue("emergency",false)) {
			unsigned char data = (SS7MsgSNM::COO == cmd) ? SS7MsgSNM::ECO : SS7MsgSNM::ECA;
			return transmitMSU(SS7MSU(txSio,lbl,&data,1),lbl,txSls) >= 0;
		    }
		    else {
			int seq = params.getIntValue("sequence",0) & 0x7f;
			int len = 2;
			unsigned char data[3];
			data[0] = cmd;
			switch (t) {
			    case SS7PointCode::ITU:
				data[1] = (unsigned char)seq;
				break;
			    case SS7PointCode::ANSI:
				data[1] = (unsigned char)((params.getIntValue("slc",sls) & 0x0f) | (seq << 4));
				data[2] = (unsigned char)(seq >> 4);
				len = 3;
				break;
			    default:
				Debug(DebugStub,"Please implement COO for type %u",t);
				return false;
			}
			return (cmd == SS7MsgSNM::COA)
			    ? transmitMSU(SS7MSU(txSio,lbl,&data,len),lbl,txSls) >= 0
			    : postpone(new SS7MSU(txSio,lbl,&data,len),lbl,txSls,1800);
		    }
		// Changeback messages
		case SS7MsgSNM::CBD:
		case SS7MsgSNM::CBA:
		    {
			int code = params.getIntValue("code",0);
			int len = 2;
			unsigned char data[3];
			data[0] = cmd;
			switch (t) {
			    case SS7PointCode::ITU:
				data[1] = (unsigned char)code;
				break;
			    case SS7PointCode::ANSI:
				data[1] = (unsigned char)((params.getIntValue("slc",sls) & 0x0f) | (code << 4));
				data[2] = (unsigned char)(code >> 4);
				len = 3;
				break;
			    default:
				Debug(DebugStub,"Please implement CBD for type %u",t);
				return false;
			}
			return (cmd == SS7MsgSNM::CBA)
			    ? transmitMSU(SS7MSU(txSio,lbl,&data,len),lbl,txSls) >= 0
			    : postpone(new SS7MSU(txSio,lbl,&data,len),lbl,txSls,1000,2000);
		    }
		default:
		    if (cmd >= 0)
			Debug(this,DebugStub,"Unimplemented control %s (%d) [%p]",
			    lookup(cmd,s_snm_names,"???"),cmd,this);
	    }
	}
    }
    TelEngine::destruct(l);
    return false;
}

void SS7Management::notify(SS7Layer3* network, int sls)
{
    Debug(this,DebugAll,"SS7Management::notify(%p,%d) [%p]",network,sls,this);
    if (network && (sls >= 0)) {
	bool linkUp = network->operational(sls);
	if (linkUp && !network->inhibited(sls,SS7Layer2::Inactive))
	    return;
	bool linkAvail[256];
	int txSls;
	for (txSls = 0; txSls < 256; txSls++)
	    linkAvail[txSls] = (txSls != sls) && !network->inhibited(txSls) && network->operational(txSls);
	for (unsigned int i = 0; i < YSS7_PCTYPE_COUNT; i++) {
	    SS7PointCode::Type type = static_cast<SS7PointCode::Type>(i+1);
	    unsigned int local = network->getLocal(type);
	    if (!local && SS7Layer4::network())
		local = SS7Layer4::network()->getLocal(type);
	    if (!local)
		continue;
	    String addr;
	    addr << SS7PointCode::lookup(type) << "," << SS7PointCode(type,local);
	    Debug(this,DebugNote,"Link %s:%d is %s [%p]",addr.c_str(),sls,
		(linkUp ? "up" : "down"),this);
	    const char* oper = linkUp ? "changeback" : "changeover";
	    ObjList* routes = getNetRoutes(network,type);
	    if (routes)
		routes = routes->skipNull();
	    for (; routes; routes = routes->skipNext()) {
		const SS7Route* r = static_cast<const SS7Route*>(routes->get());
		if (r && !r->priority()) {
		    // found adjacent node, emit change orders to it
		    int seq = -1;
		    txSls = 0;
		    if (!linkUp && network->inhibited(sls,SS7Layer2::Inactive)) {
			// already inactive, fix sequences if possible
			seq = network->getSequence(sls);
			DDebug(this,DebugAll,"Got sequence %d for link %s:%d [%p]",
			    seq,addr.c_str(),sls,this);
			if (seq < 0)
			    return;
			txSls = 256;
		    }
		    String tmp = addr;
		    tmp << "," << SS7PointCode(type,r->packed()) << "," << sls;
		    String slc(sls);
		    for (; txSls < 256; txSls++) {
			if (!linkAvail[txSls])
			    continue;
			NamedList* ctl = controlCreate(oper);
			if (!ctl)
			    continue;
			Debug(this,DebugAll,"Sending Link %d %s %s on %d [%p]",
			    sls,oper,tmp.c_str(),txSls,this);
			ctl->setParam("address",tmp);
			ctl->setParam("slc",slc);
			ctl->setParam("linksel",String(txSls));
			if (linkUp)
			    ctl->setParam("code",String(txSls));
			else {
			    if (seq < 0)
				seq = network->getSequence(sls);
			    DDebug(this,DebugAll,"Got sequence number %d [%p]",seq,this);
			    if (seq >= 0)
				ctl->setParam("sequence",String(seq));
			    else
				ctl->setParam("emergency",String::boolText(true));
			}
			ctl->setParam("automatic",String::boolText(true));
			controlExecute(ctl);
		    }
		    while (seq >= 0) {
			// scan pending list for matching ECA, turn them into COA/XCA
			SS7Label label(type,local,r->packed(),sls);
			lock();
			SnmPending* pend = 0;
			for (ObjList* l = m_pending.skipNull(); l; l = l->skipNext()) {
			    SnmPending* p = static_cast<SnmPending*>(l->get());
			    const unsigned char* ptr = p->msu().getData(p->length()+1,1);
			    if (!(ptr && p->matches(label)))
				continue;
			    if (ptr[0] != SS7MsgSNM::ECA)
				continue;
			    pend = static_cast<SnmPending*>(m_pending.remove(p,false));
			    break;
			}
			unlock();
			if (pend) {
			    const char* cmd = "COA";
			    if (seq & 0xff000000) {
				seq &= 0x00ffffff;
				cmd = "XCA";
			    }
			    Debug(this,DebugInfo,"Turning pending ECA into %s with sequence %d [%p]",
				cmd,seq,this);
			    NamedList* ctl = controlCreate(cmd);
			    if (ctl) {
				ctl->setParam("address",tmp);
				ctl->setParam("slc",slc);
				ctl->setParam("linksel",String(pend->txSls()));
				ctl->setParam("sequence",String(seq));
				ctl->setParam("automatic",String::boolText(true));
				controlExecute(ctl);
			    }
			    TelEngine::destruct(pend);
			}
			else
			    break;
		    }
		}
	    }
	}
    }
}

bool SS7Management::postpone(SS7MSU* msu, const SS7Label& label, int txSls,
	u_int64_t interval, u_int64_t global, const Time& when)
{
    if ((interval == 0) || transmitMSU(*msu,label,txSls) >= 0) {
	lock();
	m_pending.add(new SnmPending(msu,label,txSls,interval,global),when);
	unlock();
	return true;
    }
    TelEngine::destruct(msu);
    return false;
}

bool SS7Management::timeout(const SS7MSU& msu, const SS7Label& label, int txSls, bool final)
{
    Debug(this,DebugAll,"Timeout %u%s [%p]",txSls,(final ? " final" : ""),this);
    if (!final)
	return true;
    const unsigned char* buf = msu.getData(label.length()+1,1);
    if (!buf)
	return false;
    String link;
    link << SS7PointCode::lookup(label.type()) << "," << label;
    switch (buf[0]) {
	case SS7MsgSNM::COO:
	case SS7MsgSNM::XCO:
	case SS7MsgSNM::ECO:
	    Debug(this,DebugNote,"Changeover timed out on %s",link.c_str());
	    inhibit(label,SS7Layer2::Inactive);
	    break;
	case SS7MsgSNM::ECA:
	    Debug(this,DebugNote,"Emergency changeover acknowledge on %s",link.c_str());
	    transmitMSU(msu,label,txSls);
	    break;
	case SS7MsgSNM::CBD:
	    Debug(this,DebugNote,"Changeback timed out on %s",link.c_str());
	    inhibit(label,0,SS7Layer2::Inactive);
	    break;
    }
    return true;
}

bool SS7Management::timeout(SignallingMessageTimer& timer, bool final)
{
    SnmPending& msg = static_cast<SnmPending&>(timer);
    if (final) {
	String addr;
	addr << msg;
	Debug(this,DebugNote,"Expired %s control sequence to %s [%p]",
	    msg.snmName(),addr.c_str(),this);
    }
    return timeout(msg.msu(),msg,msg.txSls(),final);
}

void SS7Management::timerTick(const Time& when)
{
    for (;;) {
	lock();
	SnmPending* msg = static_cast<SnmPending*>(m_pending.timeout(when));
	unlock();
	if (!msg)
	    break;
	if (!msg->global().started() || msg->global().timeout(when.msec()))
	    timeout(*msg,true);
	else if (timeout(*msg,false)) {
	    transmitMSU(msg->msu(),*msg,msg->txSls());
	    m_pending.add(msg,when);
	    msg = 0;
	}
	TelEngine::destruct(msg);
    }
}

bool SS7Management::inhibit(const SS7Label& link, int setFlags, int clrFlags)
{
    SS7Router* router = YOBJECT(SS7Router,SS7Layer4::network());
    return router && router->inhibit(link,setFlags,clrFlags);
}

void SS7Management::recover(const SS7Label& link, int sequence)
{
    SS7Router* router = YOBJECT(SS7Router,SS7Layer4::network());
    if (router)
	router->recoverMSU(link,sequence);
}


HandledMSU SS7Maintenance::receivedMSU(const SS7MSU& msu, const SS7Label& label, SS7Layer3* network, int sls)
{
    if (msu.getSIF() != sif() && msu.getSIF() != SS7MSU::MTNS)
	return HandledMSU::Rejected;
    if (network) {
	unsigned int local = network->getLocal(label.type());
	if (local && label.dpc().pack(label.type()) != local)
	    return HandledMSU::Rejected;
    }
    XDebug(this,DebugStub,"Possibly incomplete SS7Maintenance::receivedMSU(%p,%p,%p,%d) [%p]",
	&msu,&label,network,sls,this);

    unsigned int mlen = msu.length()-label.length()-1;
    // Q.707 says test pattern length should be 1-15 but we accept 0 as well
    const unsigned char* s = msu.getData(label.length()+1,2);
    if (!s)
	return false;
    String addr;
    addr << SS7PointCode::lookup(label.type()) << "," << label;
    if (debugAt(DebugAll))
	addr << " (" << label.opc().pack(label.type()) << ":" << label.dpc().pack(label.type()) << ":" << label.sls() << ")";
    int level = DebugInfo;
    if (label.sls() != sls) {
	addr << " on " << sls;
	level = DebugMild;
    }
    unsigned char len = s[1] >> 4;
    // get a pointer to the test pattern
    const unsigned char* t = msu.getData(label.length()+3,len);
    if (!t) {
	Debug(this,DebugMild,"Received MTN %s type %02X length %u with invalid pattern length %u [%p]",
	    addr.c_str(),s[0],msu.length(),len,this);
	return false;
    }
    switch (s[0]) {
	case SS7MsgMTN::SLTM:
	    {
		Debug(this,level,"Received SLTM %s with %u bytes",addr.c_str(),len);
		SS7Label lbl(label,label.sls(),0);
		SS7MSU answer(msu.getSIO(),lbl,0,len+2);
		unsigned char* d = answer.getData(lbl.length()+1,len+2);
		if (!d)
		    return false;
		addr.clear();
		addr << SS7PointCode::lookup(lbl.type()) << "," << lbl;
		if (debugAt(DebugAll))
		    addr << " (" << lbl.opc().pack(lbl.type()) << ":" << lbl.dpc().pack(lbl.type()) << ":" << lbl.sls() << ")";
		Debug(this,DebugInfo,"Sending SLTA %s with %u bytes",addr.c_str(),len);
		*d++ = SS7MsgMTN::SLTA;
		*d++ = len << 4;
		while (len--)
		    *d++ = *t++;
		return transmitMSU(answer,lbl,lbl.sls()) >= 0;
	    }
	    return true;
	case SS7MsgMTN::SLTA:
	    Debug(this,level,"Received SLTA %s with %u bytes",addr.c_str(),len);
	    return true;
    }

    String tmp;
    tmp.hexify((void*)s,mlen,' ');
    Debug(this,DebugMild,"Unhandled MTN %s type %s length %u: %s",
	addr.c_str(),SS7MsgMTN::lookup((SS7MsgMTN::Type)s[0],"unknown"),mlen,tmp.c_str());
    return false;
}

void SS7Maintenance::notify(SS7Layer3* network, int sls)
{
}

/* vi: set ts=8 sw=4 sts=4 noet: */
