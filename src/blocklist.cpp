// merOS Shield - izleyici ve reklam engelleme
// Yalnizca sayfanin alt kaynaklari (betik, resim, stil) icin uygulanir; kullanicinin
// kendisinin gittigi adresler engellenmez.
#include "blocklist.h"

namespace mer {

namespace {
const wchar_t* kBlocked[] = {
    // Reklam aglari
    L"doubleclick.net", L"googlesyndication.com", L"googleadservices.com", L"adservice.google.com",
    L"googletagservices.com", L"amazon-adsystem.com", L"adnxs.com", L"criteo.com", L"criteo.net",
    L"taboola.com", L"outbrain.com", L"adsrvr.org", L"rubiconproject.com", L"pubmatic.com", L"openx.net",
    L"casalemedia.com", L"moatads.com", L"teads.tv", L"smartadserver.com", L"adform.net", L"media.net",
    L"yieldmo.com", L"sharethrough.com", L"3lift.com", L"bidswitch.net", L"contextweb.com", L"indexww.com",
    L"admatic.com.tr", L"adhood.com", L"popads.net", L"propellerads.com", L"exoclick.com", L"adcolony.com",
    L"zedo.com", L"revcontent.com", L"mgid.com",
    // Izleme ve analitik
    L"google-analytics.com", L"googletagmanager.com", L"analytics.google.com", L"facebook.net",
    L"connect.facebook.com", L"scorecardresearch.com", L"quantserve.com", L"hotjar.com", L"hotjar.io",
    L"mixpanel.com", L"segment.io", L"cdn.segment.com", L"nr-data.net", L"clarity.ms", L"mc.yandex.ru",
    L"bat.bing.com", L"ads-twitter.com", L"analytics.twitter.com", L"analytics.tiktok.com", L"hs-analytics.net",
    L"chartbeat.com", L"chartbeat.net", L"fullstory.com", L"mouseflow.com", L"crazyegg.com", L"luckyorange.com",
    L"inspectlet.com", L"kissmetrics.com", L"heap.io", L"heapanalytics.com", L"amplitude.com", L"branch.io",
    L"appsflyer.com", L"adjust.com", L"omtrdc.net", L"demdex.net", L"everesttech.net", L"krxd.net",
    L"bluekai.com", L"exelator.com", L"tapad.com", L"rlcdn.com", L"agkn.com", L"mathtag.com",
    L"addthis.com", L"sharethis.com", L"newrelic.com", L"pingdom.net", L"statcounter.com", L"gemius.pl",
    L"cxense.com", L"onesignal.com", L"pushwoosh.com",
};

bool EndsWithDomain(const std::wstring& host, const wchar_t* d) {
    std::wstring dom = d;
    if (host == dom) return true;
    return host.size() > dom.size() && host.compare(host.size() - dom.size(), dom.size(), dom) == 0 &&
           host[host.size() - dom.size() - 1] == L'.';
}
} // namespace

bool IsBlockedHost(const std::wstring& host) {
    for (auto* d : kBlocked) if (EndsWithDomain(host, d)) return true;
    return false;
}

} // namespace mer
