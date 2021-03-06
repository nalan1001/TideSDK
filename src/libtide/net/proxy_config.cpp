/**
* This file has been modified from its orginal sources.
*
* Copyright (c) 2012 Software in the Public Interest Inc (SPI)
* Copyright (c) 2012 David Pratt
* 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
***
* Copyright (c) 2008-2012 Appcelerator Inc.
* 
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*   http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
**/

#include "../tide.h"
#include "proxy_config.h"
#include <sstream>

#include <Poco/String.h>
#include <Poco/NumberParser.h>
#include <Poco/StringTokenizer.h>

using std::string;

#ifndef OS_OSX
#include <libproxy/proxy.h>
#endif

namespace tide
{

static std::string& ProxyTypeToString(ProxyType type)
{
    static std::string httpString("http");
    static std::string httpsString("https");
    static std::string ftpString("ftp");
    static std::string socksString("socks");
    switch (type)
    {
        case HTTP:
            return httpString;
            break;
        case HTTPS:
            return httpsString;
            break;
        case FTP:
            return ftpString;
            break;
        case SOCKS:
        default:
            return socksString;
            break;
    }
}

/*static*/
ProxyType Proxy::SchemeToProxyType(string scheme)
{
    scheme = FileUtils::Trim(scheme);
    Poco::toLowerInPlace(scheme);

    if (scheme == "https")
        return HTTPS;
    else if (scheme == "ftp")
        return FTP;
    else if (scheme == "socks")
        return SOCKS;
    else
        return HTTP;
}

std::string Proxy::ToString()
{
    std::stringstream ss;
    ss << ProxyTypeToString(type) << "://";

    if (!username.empty() || !password.empty())
        ss << username << ":" << password << "@";

    ss << host;

    if (port != 0)
        ss << ":" << port;

    return ss.str();
}

static SharedProxy GetProxyFromEnvironment(std::string prefix)
{
    Poco::toUpperInPlace(prefix);
    std::string envName = prefix + "_PROXY";

    std::string proxyString(EnvironmentUtils::Get(envName));
    if (!proxyString.empty())
        return ProxyConfig::ParseProxyEntry(proxyString, prefix, std::string());

    Poco::toLowerInPlace(prefix);
    envName = prefix + "_proxy";
    proxyString = EnvironmentUtils::Get(envName);
    if (!proxyString.empty())
        return ProxyConfig::ParseProxyEntry(proxyString, prefix, std::string());

    return 0;
}

namespace ProxyConfig
{

SharedProxy httpProxyOverride(0);
SharedProxy httpsProxyOverride(0);

void SetHTTPProxyOverride(SharedProxy newProxyOverride)
{
    httpProxyOverride = newProxyOverride;
}

SharedProxy GetHTTPProxyOverride()
{
    return httpProxyOverride;
}

void SetHTTPSProxyOverride(SharedProxy newProxyOverride)
{
    httpsProxyOverride = newProxyOverride;
}

SharedProxy GetHTTPSProxyOverride()
{
    return httpsProxyOverride;
}

SharedProxy GetProxyForURL(string& url)
{
    static Logger* logger = GetLogger();
    Poco::URI uri(url);

    // Don't try to detect proxy settings for URLs we know are local
    std::string scheme(uri.getScheme());
    if (scheme == "app" || scheme == "ti" || scheme == "file")
        return 0;

    if (scheme == "http" && !httpProxyOverride.isNull())
        return httpProxyOverride;

    if (scheme == "https" && !httpsProxyOverride.isNull())
        return httpsProxyOverride;

    SharedProxy environmentProxy(GetProxyFromEnvironment(scheme));
    if (!environmentProxy.isNull())
    {
        logger->Debug("Found proxy (%s) in environment",
            environmentProxy->ToString().c_str());
        return environmentProxy;
    }

    logger->Debug("Looking up proxy information for: %s", url.c_str());
    SharedProxy proxy(ProxyConfig::GetProxyForURLImpl(uri));

    if (proxy.isNull())
        logger->Debug("Using direct connection.");
    else
        logger->Debug("Using proxy: %s", proxy->ToString().c_str());

    return proxy;
}

static inline bool EndsWith(string haystack, string needle)
{
    return haystack.find(needle) == (haystack.size() - needle.size());
}

static bool ShouldBypassWithEntry(Poco::URI& uri, SharedPtr<BypassEntry> entry)
{
    const std::string& uriHost = uri.getHost();
    const std::string& uriScheme = uri.getScheme();
    unsigned short uriPort = uri.getPort();
    const std::string& entryHost = entry->host;
    const std::string& entryScheme = entry->scheme;
    unsigned short entryPort = entry->port;

    GetLogger()->Debug("bypass entry: scheme='%s' host='%s' port='%i'", 
        entry->scheme.c_str(), entry->host.c_str(), entry->port);

    // An empty bypass entry equals an unconditional bypass.
    if (entry.isNull())
    {
        return true;
    }
    else
    {
        if (entryHost == "<local>" && uriHost.find(".") == string::npos)
        {
            return true;
        }
        else if (EndsWith(uriHost, entryHost) &&
            (entryScheme.empty() || entryScheme == uriScheme) &&
            (entryPort == 0 || entryPort == uriPort))
        {
            return true;
        }
    }

    return false;
}

bool ShouldBypass(Poco::URI& uri, std::vector<SharedPtr<BypassEntry> >& bypassList)
{
    GetLogger()->Debug("Checking whether %s should be bypassed.", 
        uri.toString().c_str());

    for (size_t i = 0; i < bypassList.size(); i++)
    {
        if (ShouldBypassWithEntry(uri, bypassList.at(i)))
            return true;
    }

    GetLogger()->Debug("No bypass");
    return false;
}

SharedPtr<BypassEntry> ParseBypassEntry(string entry)
{
    // Traditionally an endswith comparison is always done with the host
    // part, so we throw away explicit wildcards at the beginning. If the
    // entire string is a wildcard this is an unconditional bypass.
    if (entry.at(0) == '*' && entry.size() == 1)
    {
        // Null Poco::URI means always bypass
        return 0;
    }
    else if (entry.at(0) == '*' && entry.size() > 1)
    {
        entry = entry.substr(1);
    }

    SharedPtr<BypassEntry> bypass(new BypassEntry());
    size_t endScheme = entry.find("://");
    if (endScheme != string::npos)
    {
        bypass->scheme = entry.substr(0, endScheme);
        entry = entry.substr(endScheme + 3);
    }

    size_t scan = entry.size() - 1;
    while (scan > 0 && isdigit(entry[scan]))
        scan--;

    if (entry[scan] == ':' && scan != entry.size() - 1)
    {
        string portString = entry.substr(scan + 1);
        bypass->port = atoi(portString.c_str());
        entry = entry.substr(0, scan);
    }

    bypass->host = entry;
    return bypass;
}

Logger* GetLogger()
{
    static Logger* logger = Logger::Get("Proxy");
    return logger;
}

SharedProxy ParseProxyEntry(string entry, const string& urlScheme,
    const string& entryScheme)
{
    // If the hostname came with a scheme:// speicifier, read this,
    // though it has lower precedence than the other two places the
    // scheme can be defined.
    entry = FileUtils::Trim(entry);
    size_t schemeEnd = entry.find("://");
    std::string hostScheme;
    if (schemeEnd != string::npos)
    {
        hostScheme = entry.substr(0, schemeEnd);
        entry = entry.substr(schemeEnd + 3);
    }

    // We need to pull out the credentials before the port, because
    // the port just searches for the first ':', which can be in the
    // credentials section.
    string username, password;
    size_t credentialsEnd = entry.find('@');
    if (credentialsEnd != string::npos && credentialsEnd > 0 && entry.size() > 1)
    {
        username = entry.substr(0, credentialsEnd);
        entry = entry.substr(credentialsEnd + 1);

        size_t usernameEnd = username.find(':');
        if (usernameEnd != string::npos)
        {
            password = username.substr(usernameEnd + 1);
            username = username.substr(0, usernameEnd);
        }
    }

    size_t portStart = entry.find(':');
    unsigned port = 0;
    if (portStart != string::npos && portStart != entry.size())
    {
        std::string portString(entry.substr(portStart + 1));
        entry = entry.substr(0, portStart);
        try
        {
            portString = FileUtils::Trim(portString);
            port = Poco::NumberParser::parseUnsigned(portString);
        }
        catch (Poco::SyntaxException& e)
        {
            port = 0;
        }
    }

    std::string scheme;
    if (!entryScheme.empty())
        scheme = entryScheme;
    else if (!hostScheme.empty())
        scheme = hostScheme;
    else
        scheme = urlScheme;

    if (scheme == "direct")
        return 0;

    SharedProxy proxy = new Proxy();
    proxy->type = Proxy::SchemeToProxyType(scheme);
    proxy->host = FileUtils::Trim(entry);
    proxy->port = port;

    if (!username.empty())
        proxy->username = username;
    if (!password.empty())
        proxy->password = password;

    return proxy;
}

void ParseProxyList(string proxyListString,
    std::vector<SharedProxy>& proxyList, const string& urlScheme)
{
    string sep = "; ";
    Poco::StringTokenizer proxyTokens(proxyListString, sep,
        Poco::StringTokenizer::TOK_IGNORE_EMPTY | Poco::StringTokenizer::TOK_TRIM);
    for (size_t i = 0; i < proxyTokens.count(); i++)
    {
        string entry = proxyTokens[i];

        // If this entry defines a scheme, make it override the argument.
        string entryScheme;
        size_t schemeEnd = entry.find('=');
        if (schemeEnd != string::npos)
        {
            // This proxy only applies to the scheme before '='
            entryScheme = entry.substr(0, schemeEnd);
            entry = entry.substr(schemeEnd + 1);
        }

        SharedProxy proxy(ParseProxyEntry(entry, urlScheme, entryScheme));
        GetLogger()->Debug("Proxy entry: %s", proxy->ToString().c_str());
        proxyList.push_back(proxy);
    }
}

#ifndef OS_OSX

static pxProxyFactory* GetProxyFactory()
{
				// TODO: We should free this on exit.
				static pxProxyFactory* factory = NULL;
				if (!factory)
				{
								factory = px_proxy_factory_new();
				}
				return factory;
}

SharedProxy GetProxyForURLImpl(Poco::URI& uri)
{
				std::string url(uri.toString());
				char* urlC = strdup(url.c_str());
				char** proxies = px_proxy_factory_get_proxies(GetProxyFactory(), urlC);
				free(urlC);

				// TODO: Instead of just returning the first applicable proxy, this
				// should return a list of them.
				const char* proxyChars = proxies[0];
				if (!proxyChars)
								return 0;

				// Do not pass in an entryScheme here (third argument), because it will
				// override the host scheme, which is the most important in this case.
				SharedProxy proxy(ProxyConfig::ParseProxyEntry(
																proxyChars, uri.getScheme(), std::string()));

				for (int i = 0; proxies[i]; i++)
								free(proxies[i]);
				free(proxies);

				return proxy;
}
#endif
} // namespace ProxyConfig
} // namespace tide
