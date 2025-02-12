﻿/*
 * MIT License
 *
 * Copyright (c) 2016-2019 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */


#include "RtmpSession.h"
#include "Common/config.h"
#include "Util/onceToken.h"

namespace mediakit {

static int kSockFlags = SOCKET_DEFAULE_FLAGS | FLAG_MORE;

RtmpSession::RtmpSession(const Socket::Ptr &pSock) : TcpSession(pSock) {
	DebugP(this);
    GET_CONFIG(uint32_t,keep_alive_sec,Rtmp::kKeepAliveSecond);
    pSock->setSendTimeOutSecond(keep_alive_sec);
    //起始接收buffer缓存设置为4K，节省内存
    pSock->setReadBuffer(std::make_shared<BufferRaw>(4 * 1024));
}

RtmpSession::~RtmpSession() {
    DebugP(this);
}

void RtmpSession::onError(const SockException& err) {
	WarnP(this) << err.what();

    //流量统计事件广播
    GET_CONFIG(uint32_t,iFlowThreshold,General::kFlowThreshold);

    if(_ui64TotalBytes > iFlowThreshold * 1024){
        bool isPlayer = !_pPublisherSrc;
        NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastFlowReport,
                                           _mediaInfo,
                                           _ui64TotalBytes,
                                           _ticker.createdTime()/1000,
                                           isPlayer,
                                           *this);
    }
}

void RtmpSession::onManager() {
    GET_CONFIG(uint32_t,handshake_sec,Rtmp::kKeepAliveSecond);
    GET_CONFIG(uint32_t,keep_alive_sec,Rtmp::kKeepAliveSecond);

	if (_ticker.createdTime() > handshake_sec * 1000) {
		if (!_pRingReader && !_pPublisherSrc) {
			shutdown(SockException(Err_timeout,"illegal connection"));
		}
	}
	if (_pPublisherSrc) {
		//publisher
		if (_ticker.elapsedTime() > keep_alive_sec * 1000) {
			shutdown(SockException(Err_timeout,"recv data from rtmp pusher timeout"));
		}
	}
}

void RtmpSession::onRecv(const Buffer::Ptr &pBuf) {
	_ticker.resetTime();
	try {
        _ui64TotalBytes += pBuf->size();
		onParseRtmp(pBuf->data(), pBuf->size());
	} catch (exception &e) {
		shutdown(SockException(Err_shutdown, e.what()));
	}
}

void RtmpSession::onCmd_connect(AMFDecoder &dec) {
	auto params = dec.load<AMFValue>();
	double amfVer = 0;
	AMFValue objectEncoding = params["objectEncoding"];
	if(objectEncoding){
		amfVer = objectEncoding.as_number();
	}
	///////////set chunk size////////////////
	sendChunkSize(60000);
	////////////window Acknowledgement size/////
	sendAcknowledgementSize(5000000);
	///////////set peerBandwidth////////////////
	sendPeerBandwidth(5000000);

    _mediaInfo._app = params["app"].as_string();
    _strTcUrl = params["tcUrl"].as_string();
    if(_strTcUrl.empty()){
        //defaultVhost:默认vhost
        _strTcUrl = string(RTMP_SCHEMA) + "://" + DEFAULT_VHOST + "/" + _mediaInfo._app;
    }
	bool ok = true; //(app == APP_NAME);
	AMFValue version(AMF_OBJECT);
	version.set("fmsVer", "FMS/3,0,1,123");
	version.set("capabilities", 31.0);
	AMFValue status(AMF_OBJECT);
	status.set("level", ok ? "status" : "error");
	status.set("code", ok ? "NetConnection.Connect.Success" : "NetConnection.Connect.InvalidApp");
	status.set("description", ok ? "Connection succeeded." : "InvalidApp.");
	status.set("objectEncoding", amfVer);
	sendReply(ok ? "_result" : "_error", version, status);
	if (!ok) {
		throw std::runtime_error("Unsupported application: " + _mediaInfo._app);
	}

	AMFEncoder invoke;
	invoke << "onBWDone" << 0.0 << nullptr;
	sendResponse(MSG_CMD, invoke.data());
}

void RtmpSession::onCmd_createStream(AMFDecoder &dec) {
	sendReply("_result", nullptr, double(STREAM_MEDIA));
}

void RtmpSession::onCmd_publish(AMFDecoder &dec) {
    std::shared_ptr<Ticker> pTicker(new Ticker);
    weak_ptr<RtmpSession> weakSelf = dynamic_pointer_cast<RtmpSession>(shared_from_this());
    std::shared_ptr<onceToken> pToken(new onceToken(nullptr,[pTicker,weakSelf](){
        auto strongSelf = weakSelf.lock();
        if(strongSelf){
            DebugP(strongSelf.get()) << "publish 回复时间:" << pTicker->elapsedTime() << "ms";
        }
    }));
	dec.load<AMFValue>();/* NULL */
    _mediaInfo.parse(_strTcUrl + "/" + dec.load<std::string>());
    _mediaInfo._schema = RTMP_SCHEMA;

    auto onRes = [this,pToken](const string &err){
        auto src = dynamic_pointer_cast<RtmpMediaSource>(MediaSource::find(RTMP_SCHEMA,
                                                                           _mediaInfo._vhost,
                                                                           _mediaInfo._app,
                                                                           _mediaInfo._streamid,
                                                                           false));
        bool authSuccess = err.empty();
        bool ok = (!src && !_pPublisherSrc && authSuccess);
        AMFValue status(AMF_OBJECT);
        status.set("level", ok ? "status" : "error");
        status.set("code", ok ? "NetStream.Publish.Start" : (authSuccess ? "NetStream.Publish.BadName" : "NetStream.Publish.BadAuth"));
        status.set("description", ok ? "Started publishing stream." : (authSuccess ? "Already publishing." : err.data()));
        status.set("clientid", "0");
        sendReply("onStatus", nullptr, status);
        if (!ok) {
            string errMsg = StrPrinter << (authSuccess ? "already publishing:" : err.data()) << " "
                                    << _mediaInfo._vhost << " "
                                    << _mediaInfo._app << " "
                                    << _mediaInfo._streamid;
            shutdown(SockException(Err_shutdown,errMsg));
            return;
        }
        _pPublisherSrc.reset(new RtmpToRtspMediaSource(_mediaInfo._vhost,_mediaInfo._app,_mediaInfo._streamid));
        _pPublisherSrc->setListener(dynamic_pointer_cast<MediaSourceEvent>(shared_from_this()));
        //如果是rtmp推流客户端，那么加大TCP接收缓存，这样能提升接收性能
        _sock->setReadBuffer(std::make_shared<BufferRaw>(256 * 1024));
    };

    Broadcast::AuthInvoker invoker = [weakSelf,onRes,pToken](const string &err){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        strongSelf->async([weakSelf,onRes,err,pToken](){
            auto strongSelf = weakSelf.lock();
            if(!strongSelf){
                return;
            }
            onRes(err);
        });
    };
    auto flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPublish,
                                                   _mediaInfo,
                                                   invoker,
                                                   *this);
    if(!flag){
        //该事件无人监听，默认鉴权成功
        onRes("");
    }
}

void RtmpSession::onCmd_deleteStream(AMFDecoder &dec) {
	AMFValue status(AMF_OBJECT);
	status.set("level", "status");
	status.set("code", "NetStream.Unpublish.Success");
	status.set("description", "Stop publishing.");
	sendReply("onStatus", nullptr, status);
	throw std::runtime_error(StrPrinter << "Stop publishing." << endl);
}


void RtmpSession::sendPlayResponse(const string &err,const RtmpMediaSource::Ptr &src){
    bool authSuccess = err.empty();
    bool ok = (src.operator bool() && authSuccess);
    if (ok) {
        //stream begin
        sendUserControl(CONTROL_STREAM_BEGIN, STREAM_MEDIA);
    }
    // onStatus(NetStream.Play.Reset)
    AMFValue status(AMF_OBJECT);
    status.set("level", ok ? "status" : "error");
    status.set("code", ok ? "NetStream.Play.Reset" : (authSuccess ? "NetStream.Play.StreamNotFound" : "NetStream.Play.BadAuth"));
    status.set("description", ok ? "Resetting and playing." : (authSuccess ? "No such stream." : err.data()));
    status.set("details", _mediaInfo._streamid);
    status.set("clientid", "0");
    sendReply("onStatus", nullptr, status);
    if (!ok) {
        string errMsg = StrPrinter << (authSuccess ? "no such stream:" : err.data()) << " "
                                 << _mediaInfo._vhost << " "
                                 << _mediaInfo._app << " "
                                 << _mediaInfo._streamid;
        shutdown(SockException(Err_shutdown,errMsg));
        return;
    }

    // onStatus(NetStream.Play.Start)
    status.clear();
    status.set("level", "status");
    status.set("code", "NetStream.Play.Start");
    status.set("description", "Started playing.");
    status.set("details", _mediaInfo._streamid);
    status.set("clientid", "0");
    sendReply("onStatus", nullptr, status);

    // |RtmpSampleAccess(true, true)
    AMFEncoder invoke;
    invoke << "|RtmpSampleAccess" << true << true;
    sendResponse(MSG_DATA, invoke.data());

    //onStatus(NetStream.Data.Start)
    invoke.clear();
    AMFValue obj(AMF_OBJECT);
    obj.set("code", "NetStream.Data.Start");
    invoke << "onStatus" << obj;
    sendResponse(MSG_DATA, invoke.data());

    //onStatus(NetStream.Play.PublishNotify)
    status.clear();
    status.set("level", "status");
    status.set("code", "NetStream.Play.PublishNotify");
    status.set("description", "Now published.");
    status.set("details", _mediaInfo._streamid);
    status.set("clientid", "0");
    sendReply("onStatus", nullptr, status);

    // onMetaData
    invoke.clear();
    invoke << "onMetaData" << src->getMetaData();
    sendResponse(MSG_DATA, invoke.data());

    src->getConfigFrame([&](const RtmpPacket::Ptr &pkt) {
        //DebugP(this)<<"send initial frame";
        onSendMedia(pkt);
    });

    _pRingReader = src->getRing()->attach(getPoller());
    weak_ptr<RtmpSession> weakSelf = dynamic_pointer_cast<RtmpSession>(shared_from_this());
    SockUtil::setNoDelay(_sock->rawFD(), false);
    _pRingReader->setReadCB([weakSelf](const RtmpPacket::Ptr &pkt) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }
        strongSelf->onSendMedia(pkt);
    });
    _pRingReader->setDetachCB([weakSelf]() {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }
        strongSelf->shutdown(SockException(Err_shutdown,"rtmp ring buffer detached"));
    });
    _pPlayerSrc = src;
    if (src->readerCount() == 1) {
        src->seekTo(0);
    }

    //提高发送性能
    (*this) << SocketFlags(kSockFlags);
    SockUtil::setNoDelay(_sock->rawFD(),false);
}

void RtmpSession::doPlayResponse(const string &err,const std::function<void(bool)> &cb){
    if(!err.empty()){
        //鉴权失败，直接返回播放失败
        sendPlayResponse(err, nullptr);
        cb(false);
        return;
    }

    //鉴权成功，查找媒体源并回复
    weak_ptr<RtmpSession> weakSelf = dynamic_pointer_cast<RtmpSession>(shared_from_this());
    MediaSource::findAsync(_mediaInfo,weakSelf.lock(), true,[weakSelf,cb](const MediaSource::Ptr &src){
        auto rtmp_src = dynamic_pointer_cast<RtmpMediaSource>(src);
        auto strongSelf = weakSelf.lock();
        if(strongSelf){
            strongSelf->sendPlayResponse("", rtmp_src);
        }
        cb(rtmp_src.operator bool());
    });
}

void RtmpSession::doPlay(AMFDecoder &dec){
    std::shared_ptr<Ticker> pTicker(new Ticker);
    weak_ptr<RtmpSession> weakSelf = dynamic_pointer_cast<RtmpSession>(shared_from_this());
    std::shared_ptr<onceToken> pToken(new onceToken(nullptr,[pTicker,weakSelf](){
        auto strongSelf = weakSelf.lock();
        if(strongSelf) {
            DebugP(strongSelf.get()) << "play 回复时间:" << pTicker->elapsedTime() << "ms";
        }
    }));
    Broadcast::AuthInvoker invoker = [weakSelf,pToken](const string &err){
        auto strongSelf = weakSelf.lock();
        if(!strongSelf){
            return;
        }
        strongSelf->async([weakSelf,err,pToken](){
            auto strongSelf = weakSelf.lock();
            if(!strongSelf){
                return;
            }
            strongSelf->doPlayResponse(err,[pToken](bool){});
        });
    };
    auto flag = NoticeCenter::Instance().emitEvent(Broadcast::kBroadcastMediaPlayed,_mediaInfo,invoker,*this);
    if(!flag){
        //该事件无人监听,默认不鉴权
        doPlayResponse("",[pToken](bool){});
    }
}
void RtmpSession::onCmd_play2(AMFDecoder &dec) {
	doPlay(dec);
}
void RtmpSession::onCmd_play(AMFDecoder &dec) {
	dec.load<AMFValue>();/* NULL */
    _mediaInfo.parse(_strTcUrl + "/" + dec.load<std::string>());
    _mediaInfo._schema = RTMP_SCHEMA;
	doPlay(dec);
}

void RtmpSession::onCmd_pause(AMFDecoder &dec) {
	dec.load<AMFValue>();/* NULL */
	bool paused = dec.load<bool>();
	TraceP(this) << paused;
	AMFValue status(AMF_OBJECT);
	status.set("level", "status");
	status.set("code", paused ? "NetStream.Pause.Notify" : "NetStream.Unpause.Notify");
	status.set("description", paused ? "Paused stream." : "Unpaused stream.");
	sendReply("onStatus", nullptr, status);
//streamBegin
	sendUserControl(paused ? CONTROL_STREAM_EOF : CONTROL_STREAM_BEGIN,
	STREAM_MEDIA);
	if (!_pRingReader) {
		throw std::runtime_error("Rtmp not started yet!");
	}
	if (paused) {
		_pRingReader->setReadCB(nullptr);
	} else {
		weak_ptr<RtmpSession> weakSelf = dynamic_pointer_cast<RtmpSession>(shared_from_this());
		_pRingReader->setReadCB([weakSelf](const RtmpPacket::Ptr &pkt) {
			auto strongSelf = weakSelf.lock();
			if(!strongSelf) {
				return;
			}
            strongSelf->onSendMedia(pkt);
		});
	}
}

void RtmpSession::setMetaData(AMFDecoder &dec) {
	if (!_pPublisherSrc) {
		throw std::runtime_error("not a publisher");
	}
	std::string type = dec.load<std::string>();
	if (type != "onMetaData") {
		throw std::runtime_error("can only set metadata");
	}
	_pPublisherSrc->onGetMetaData(dec.load<AMFValue>());
}

void RtmpSession::onProcessCmd(AMFDecoder &dec) {
    typedef void (RtmpSession::*rtmpCMDHandle)(AMFDecoder &dec);
    static unordered_map<string, rtmpCMDHandle> g_mapCmd;
    static onceToken token([]() {
        g_mapCmd.emplace("connect",&RtmpSession::onCmd_connect);
        g_mapCmd.emplace("createStream",&RtmpSession::onCmd_createStream);
        g_mapCmd.emplace("publish",&RtmpSession::onCmd_publish);
        g_mapCmd.emplace("deleteStream",&RtmpSession::onCmd_deleteStream);
        g_mapCmd.emplace("play",&RtmpSession::onCmd_play);
        g_mapCmd.emplace("play2",&RtmpSession::onCmd_play2);
        g_mapCmd.emplace("seek",&RtmpSession::onCmd_seek);
        g_mapCmd.emplace("pause",&RtmpSession::onCmd_pause);}, []() {});

    std::string method = dec.load<std::string>();
	auto it = g_mapCmd.find(method);
	if (it == g_mapCmd.end()) {
		TraceP(this) << "can not support cmd:" << method;
		return;
	}
	_dNowReqID = dec.load<double>();
	auto fun = it->second;
	(this->*fun)(dec);
}

void RtmpSession::onRtmpChunk(RtmpPacket &chunkData) {
	switch (chunkData.typeId) {
	case MSG_CMD:
	case MSG_CMD3: {
		AMFDecoder dec(chunkData.strBuf, chunkData.typeId == MSG_CMD3 ? 1 : 0);
		onProcessCmd(dec);
	}
		break;

	case MSG_DATA:
	case MSG_DATA3: {
		AMFDecoder dec(chunkData.strBuf, chunkData.typeId == MSG_CMD3 ? 1 : 0);
		std::string type = dec.load<std::string>();
		TraceP(this) << "notify:" << type;
		if (type == "@setDataFrame") {
			setMetaData(dec);
		}
	}
		break;
	case MSG_AUDIO:
	case MSG_VIDEO: {
		if (!_pPublisherSrc) {
			throw std::runtime_error("Not a rtmp publisher!");
		}
		GET_CONFIG(bool,rtmp_modify_stamp,Rtmp::kModifyStamp);
		if(rtmp_modify_stamp){
			chunkData.timeStamp = _stampTicker[chunkData.typeId % 2].elapsedTime();
		}
		_pPublisherSrc->onWrite(std::make_shared<RtmpPacket>(std::move(chunkData)));
	}
		break;
	default:
		WarnP(this) << "unhandled message:" << (int) chunkData.typeId << hexdump(chunkData.strBuf.data(), chunkData.strBuf.size());
		break;
	}
}

void RtmpSession::onCmd_seek(AMFDecoder &dec) {
    dec.load<AMFValue>();/* NULL */
    auto milliSeconds = dec.load<AMFValue>().as_number();
    InfoP(this) << "rtmp seekTo(ms):" << milliSeconds;
    auto stongSrc = _pPlayerSrc.lock();
    if (stongSrc) {
        stongSrc->seekTo(milliSeconds);
    }
	AMFValue status(AMF_OBJECT);
	AMFEncoder invoke;
	status.set("level", "status");
	status.set("code", "NetStream.Seek.Notify");
	status.set("description", "Seeking.");
	sendReply("onStatus", nullptr, status);
}

void RtmpSession::onSendMedia(const RtmpPacket::Ptr &pkt) {
	auto modifiedStamp = pkt->timeStamp;
	auto &firstStamp = _aui32FirstStamp[pkt->typeId % 2];
	if(!firstStamp){
		firstStamp = modifiedStamp;
	}
	if(modifiedStamp >= firstStamp){
		//计算时间戳增量
		modifiedStamp -= firstStamp;
	}else{
		//发生回环，重新计算时间戳增量
		CLEAR_ARR(_aui32FirstStamp);
		modifiedStamp = 0;
	}
	sendRtmp(pkt->typeId, pkt->streamId, pkt, modifiedStamp, pkt->chunkId);
}


bool RtmpSession::close(MediaSource &sender,bool force)  {
    //此回调在其他线程触发
    if(!_pPublisherSrc || (!force && _pPublisherSrc->readerCount() != 0)){
        return false;
    }
    string err = StrPrinter << "close media:" << sender.getSchema() << "/" << sender.getVhost() << "/" << sender.getApp() << "/" << sender.getId() << " " << force;
    safeShutdown(SockException(Err_shutdown,err));
    return true;
}

void RtmpSession::onNoneReader(MediaSource &sender) {
    //此回调在其他线程触发
    if(!_pPublisherSrc || _pPublisherSrc->readerCount() != 0){
        return;
    }
    MediaSourceEvent::onNoneReader(sender);
}

} /* namespace mediakit */
