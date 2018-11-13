﻿/*
* MIT License
*
* Copyright (c) 2016 xiongziliang <771730766@qq.com>
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

#include "HttpClient.h"
#include "Rtsp/Rtsp.h"

namespace mediakit {


HttpClient::HttpClient() {
}

HttpClient::~HttpClient() {
}

void HttpClient::sendRequest(const string &strUrl, float fTimeOutSec) {
    _aliveTicker.resetTime();
    auto protocol = FindField(strUrl.data(), NULL, "://");
    uint16_t defaultPort;
    bool isHttps;
    if (strcasecmp(protocol.data(), "http") == 0) {
        defaultPort = 80;
        isHttps = false;
    } else if (strcasecmp(protocol.data(), "https") == 0) {
        defaultPort = 443;
        isHttps = true;
    } else {
        auto strErr = StrPrinter << "非法的协议:" << protocol << endl;
        throw std::invalid_argument(strErr);
    }

    auto host = FindField(strUrl.data(), "://", "/");
    if (host.empty()) {
        host = FindField(strUrl.data(), "://", NULL);
    }
    _path = FindField(strUrl.data(), host.data(), NULL);
    if (_path.empty()) {
        _path = "/";
    }
    auto port = atoi(FindField(host.data(), ":", NULL).data());
    if (port <= 0) {
        //默认端口
        port = defaultPort;
    } else {
        //服务器域名
        host = FindField(host.data(), NULL, ":");
    }
    _header.emplace(string("Host"), host);
    _header.emplace(string("Tools"), "ZLMediaKit");
    _header.emplace(string("Connection"), "keep-alive");
    _header.emplace(string("Accept"), "*/*");
    _header.emplace(string("Accept-Language"), "zh-CN,zh;q=0.8");
    _header.emplace(string("User-Agent"),
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_12_1) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/57.0.2987.133 Safari/537.36");

    if (_body && _body->remainSize()) {
        _header.emplace(string("Content-Length"), to_string(_body->remainSize()));
        _header.emplace(string("Content-Type"), "application/x-www-form-urlencoded; charset=UTF-8");
    }

    bool bChanged = (_lastHost != host + ":" + to_string(port)) || (_isHttps != isHttps);
    _lastHost = host + ":" + to_string(port);
    _isHttps = isHttps;
    _fTimeOutSec = fTimeOutSec;

    auto cookies = HttpCookieStorage::Instance().get(_lastHost,_path);
    _StrPrinter printer;
    for(auto &cookie : cookies){
        printer << cookie->getKey() << "=" << cookie->getVal() << ";";
    }
    if(!printer.empty()){
        printer.pop_back();
        _header.emplace(string("Cookie"), printer);
    }


    if (!alive() || bChanged) {
        //InfoL << "reconnet:" << _lastHost;
        startConnect(host, port, fTimeOutSec);
    } else {
        SockException ex;
        onConnect(ex);
    }
}


void HttpClient::onConnect(const SockException &ex) {
    _aliveTicker.resetTime();
    if (ex) {
        onDisconnect(ex);
        return;
    }

    _totalBodySize = 0;
    _recvedBodySize = 0;
    HttpRequestSplitter::reset();

    _StrPrinter printer;
    printer << _method + " " << _path + " HTTP/1.1\r\n";
    for (auto &pr : _header) {
        printer << pr.first + ": ";
        printer << pr.second + "\r\n";
    }
    send(printer << "\r\n");
    onSend();
}

void HttpClient::onRecv(const Buffer::Ptr &pBuf) {
    onRecvBytes(pBuf->data(), pBuf->size());
}

void HttpClient::onRecvBytes(const char *data, int size) {
    _aliveTicker.resetTime();
    HttpRequestSplitter::input(data, size);
}

void HttpClient::onErr(const SockException &ex) {
    if (ex.getErrCode() == Err_eof && _totalBodySize < 0) {
        //如果Content-Length未指定 但服务器断开链接
        //则认为本次http请求完成
        onResponseCompleted_l();
    }
    onDisconnect(ex);
}

int64_t HttpClient::onRecvHeader(const char *data, uint64_t len) {
    _parser.Parse(data);
    checkCookie(_parser.getValues());
    _totalBodySize = onResponseHeader(_parser.Url(), _parser.getValues());

    if(!_parser["Content-Length"].empty()){
        //有Content-Length字段时忽略onResponseHeader的返回值
        _totalBodySize = atoll(_parser["Content-Length"].data());
    }

    if(_totalBodySize == 0){
        //后续没content，本次http请求结束
        onResponseCompleted_l();
        return 0;
    }

    //当_totalBodySize != 0时到达这里，代表后续有content
    //虽然我们在_totalBodySize >0 时知道content的确切大小，
    //但是由于我们没必要等content接收完毕才回调onRecvContent(因为这样浪费内存并且要多次拷贝数据)
    //所以返回-1代表我们接下来分段接收content
    _recvedBodySize = 0;
    return -1;
}

void HttpClient::onRecvContent(const char *data, uint64_t len) {
    auto recvedBodySize = _recvedBodySize + len;
    if(_totalBodySize < 0){
        //不限长度的content,最大支持INT64_MAX个字节
        onResponseBody(data, len, recvedBodySize, INT64_MAX);
        _recvedBodySize = recvedBodySize;
        return;
    }

    //固定长度的content
    if ( recvedBodySize < _totalBodySize ) {
        //content还未接收完毕
        onResponseBody(data, len, recvedBodySize, _totalBodySize);
        _recvedBodySize = recvedBodySize;
        return;
    }

    //content接收完毕
    onResponseBody(data, _totalBodySize - _recvedBodySize, _totalBodySize, _totalBodySize);
    bool biggerThanExpected = recvedBodySize > _totalBodySize;
    onResponseCompleted_l();
    if(biggerThanExpected) {
        //声明的content数据比真实的小，那么我们只截取前面部分的并断开链接
        shutdown();
        onDisconnect(SockException(Err_other, "http response content size bigger than expected"));
    }
}

void HttpClient::onSend() {
    _aliveTicker.resetTime();
    while (_body && _body->remainSize() && !isSocketBusy()) {
        auto buffer = _body->readData();
        if (!buffer) {
            //数据发送结束或读取数据异常
            break;
        }
        if (send(buffer) <= 0) {
            //发送数据失败，不需要回滚数据，因为发送前已经通过isSocketBusy()判断socket可写
            //所以发送缓存区肯定未满,该buffer肯定已经写入socket
            break;
        }
    }
}

void HttpClient::onManager() {
    if (_aliveTicker.elapsedTime() > 3 * 1000 && _totalBodySize < 0) {
        //如果Content-Length未指定 但接收数据超时
        //则认为本次http请求完成
        onResponseCompleted_l();
    }

    if (_fTimeOutSec > 0 && _aliveTicker.elapsedTime() > _fTimeOutSec * 1000) {
        //超时
        onDisconnect(SockException(Err_timeout, "http request timeout"));
        shutdown();
    }
}

void HttpClient::onResponseCompleted_l() {
    _totalBodySize = 0;
    _recvedBodySize = 0;
    HttpRequestSplitter::reset();
    onResponseCompleted();
}

void HttpClient::checkCookie(const HttpClient::HttpHeader &headers) {
    //Set-Cookie: IPTV_SERVER=8E03927B-CC8C-4389-BC00-31DBA7EC7B49;expires=Sun, Sep 23 2018 15:07:31 GMT;path=/index/api/
    auto it_set_cookie = headers.find("Set-Cookie");
    if(it_set_cookie == headers.end()){
        return;
    }
    auto key_val = Parser::parseArgs(it_set_cookie->second,";","=");

    HttpCookie::Ptr cookie = std::make_shared<HttpCookie>();
    cookie->setHost(_lastHost);

    int index = 0;
    auto arg_vec = split(it_set_cookie->second, ";");
    for (string &key_val : arg_vec) {
        auto key = FindField(key_val.data(),NULL,"=");
        auto val = FindField(key_val.data(),"=", NULL);

        if(index++ == 0){
            cookie->setKeyVal(key,val);
        } else{
            if(key == "path"){
                cookie->setPath(val);
            }else if(key == "expires"){
                cookie->setExpires(val);
            }
        }
    }

    if(!(*cookie)){
        //无效的cookie
        return;
    }
    HttpCookieStorage::Instance().set(cookie);
}


} /* namespace mediakit */

