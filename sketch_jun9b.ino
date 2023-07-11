/**********************************************************************
项目名称/Project          : 仅使用ESP32-C3实现支付宝当面付解决方案
程序名称/Program name     : Alipayf2f
团队/Team                : 青岛迷之科技有限公司 /  (www.mizhikeji.com)
作者/Author              : 迷之科技点灯开发组（李工）
日期/Date（YYYYMMDD）     : 2023/06/09
程序目的/Purpose          : 仅使用ESP32C3实现客户访问设备IP实现当面付，付款成功后驱动LED闪烁模拟开门
***********************************************************************/
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "FS.h"
#include <LittleFS.h>
// #include <esp_task_wdt.h>

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#define FORMAT_LITTLEFS_IF_FAILED true
//加密算法库
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/pem.h>       //https://blog.csdn.net/qdlyd/article/details/131084726?spm=1001.2014.3001.5501
#include <mbedtls/sha256.h>    //https://blog.csdn.net/imba_wolf/article/details/122417540
#include <mbedtls/ctr_drbg.h>  //https://www.trustedfirmware.org/projects/mbed-tls/
#include <mbedtls/entropy.h>   //https://johanneskinzig.de/index.php/files/26/Arduino-mbedtls/9/gettingstartedmbedtlsarduino.7z
#include <arduino_base64.hpp>  //https://github.com/dojyorin/arduino_base64 随便拉的库 库管理搜base64_encode作者dojyorin
//URL处理
#include <UrlEncode.h>
//UDPz底层库
#include <AsyncUDP.h>  //https://github.com/me-no-dev/ESPAsyncUDP
//HTTP客户端
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
//JSON编解码
#include <Arduino_JSON.h>
//时间函数
#define SECS_PER_MIN ((time_t)(60UL))
#define SECS_PER_HOUR ((time_t)(3600UL))
#define SECS_PER_DAY ((time_t)(SECS_PER_HOUR * 24UL))
#define LEAP_YEAR(Y) (((1970 + (Y)) > 0) && !((1970 + (Y)) % 4) && (((1970 + (Y)) % 100) || !((1970 + (Y)) % 400)))
static const uint8_t monthDays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };  // API starts months from 1, this array starts from 0




AsyncUDP udp;
char *ntpServer = "pool.ntp.org";  //time.nist.gov只有协议3 pool.ntp.org CN.NTP.ORG.CN
const unsigned int localPort = 23511;
unsigned long long ntpTime = 0;  //最后一次对时距离1900年的秒数
unsigned long timesendudp;
unsigned long timegetudp;
bool needupadte = 1;
//定义了一种传统格里高利时间的结构体
typedef struct {
  uint8_t Second;
  uint8_t Minute;
  uint8_t Hour;
  uint8_t Wday;  // day of week, Sunday is day 1
  uint8_t Day;
  uint8_t Month;
  uint32_t Year;  // offset from 1970;END AY 4294967295YEAR
} tmElement64s_t;

//定义WIFI配置
const char *ssid = "S23";  //
const char *password = "12345678";
const char *dataFile = "/data.txt";  // 存储键值对的文件路径
String websetpassword = "123";       //网页配置密码 1821行

const int LED_PIN = 13;
const int ERROE_PIN = 12;
int i = 1;
int webpassworderrortime = 0;  //设置密码测试计时器
int alipayneedasktime = 0;     //支付宝轮询剩余秒数
int tresttime = 0;             //支付宝轮询剩余秒数
int ORDERNUM = 0;              //支付宝轮询剩余秒数

String price[19];             //定义存储价格的价格内存 全局变量 因为浮点数据存在小数分辨率问题
int doorio[19];               //定义一个储存门对应IO的全局数组
String Alipaysign[19];        //定义一个签名字符串数组
uint64_t Alipaysingtime[19];  //定义一个支付宝签名时间数组，用于复原原有签出请求URL
String Alipaylastorder;       //最后轮询的订单号
int alipaylastdoor;           //使用支付宝渠道的最后提交支付的门号
uint64_t alipaydealtime = 0;  //订单生成器种子
uint64_t alipaysigntime[19];  //已签名的订单号
String alipayrootcertsn;
String alipayappcertsn;
String alipayappid;
String alipayprivatekey;      //私钥 在内存不足时应使用局部变量
int alipyneedsigndoor = 1;    //轮询生成请求字符串
String alipayneedcloseorder;  //由于支付宝存在先天BUG，无法关闭未创建的订单，也就是预创建订单，所以轮询关单
//再多的防备，也做不出没有漏洞的系统，如果有人同时生成多笔订单并关闭机器导致线程关闭，再开机也只会关闭最后一笔订单。因此产生的跳单需要商户自行处理
uint64_t alipayneedclosetrytime = 0;  //关闭订单轮询结束时间戳
uint64_t alipayneedquerytrytime = 0;  //查询订单轮询结束时间戳
String alipayneedqueryorder;          //当前需要轮询的订单号
String lastsuccessalipayorder;        //最后成交订单号用于误关门重开
TaskHandle_t task1Handle;


AsyncWebServer server(80);
const char *rootCACertificate =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh\n"
  "MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3\n"
  "d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD\n"
  "QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT\n"
  "MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j\n"
  "b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG\n"
  "9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB\n"
  "CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97\n"
  "nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt\n"
  "43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P\n"
  "T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4\n"
  "gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO\n"
  "BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR\n"
  "TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw\n"
  "DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr\n"
  "hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg\n"
  "06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF\n"
  "PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls\n"
  "YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk\n"
  "CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=\n"
  "-----END CERTIFICATE-----\n";
const char *index_html = R"html(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>售货机-青岛迷之科技提供软件支持</title>
  <style>
    button {
      padding: 10px 20px;
      font-size: 16px;
      margin-bottom: 10px;
    }
  </style>
</head>
<body>
  <h1>点击按钮支付宝购买<br>注意设备序列号为<br>
)html";

const char *index_html2 = R"html( 
  
  
  <button onclick="sendControlRequest('on')">开启</button>
  <button onclick="sendControlRequest('off')">关闭</button><br>
  <a href="set">设备后台设置</a>
  <script>
    function sendControlRequest(state) {
      var xhr = new XMLHttpRequest();
      xhr.open("GET", "/control?state=" + state, true);
      xhr.send();
    }
  </script>
</body>
</html>)html";




//alipayprivatekey
const char *setwebpassword_html = R"html(<!DOCTYPE html><html><head><meta charset="utf-8"><title>设置密码页</title></head><body><h1>设备密码设置</h1><form action="setwebpassword2"id="f1">原密码<input type="text"id="input1"name="oldpassword"><br>新密码<input type="text"id="input2"name="newpassword"><br></form><button onclick="t()">提交更改</button><!--<input value="提交">-->忘记密码，联系厂家<br><br><br><a href="/"><button>返回主页</button></a><script>function t(){document.getElementById("input1").value=document.getElementById("input1").value.trim();document.getElementById("input2").value=document.getElementById("input2").value.trim();if(/\s/g.test(document.getElementById("input2").value)){alert("不能有空格！！");return true}if(document.getElementById("input2").value.length<=16){document.getElementById('f1').submit()}else{alert("密码过长（需要小于16位）")}}</script></body></html><!--)html";

const char *set_html = R"html(<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>本地设置页</title>
  <style>
    button {
      padding: 10px 20px;
      font-size: 16px;
      margin-bottom: 10px;
    }
  </style>
</head>
<body>
  <h1>设备设置</h1>
  <a href="setwebpassword"><button>设置设备管理密码</button></a><br>
  <a href="setalipayprivatekey"><button>设置支付宝参数</button></a><br>
  <a href="setalipaypublickey"><button>设置支付宝HTTPS公钥</button></a><br>
  <a href="setprice"><button>设置价格</button></a><br>
  <a href="time"><button>设置时间</button></a><br>
  <a href="setdoorpin"><button>设置门与物理IO管脚对应关系</button></a><br>
  <a href="/"><button>返回主页</button></a>
</body>
</html><!--
)html";



const char *setprice_html = R"html(<!DOCTYPE html>
<html>

<head>
    <meta charset="utf-8">
    <meta HTTP-EQUIV="pragma" CONTENT="no-cache"> 
    <title>设置价格页</title>
</head>

<body>
    <h1>设置价格页</h1>
    <h1>价格0元代表关闭购买按钮</h1>
    <h1>负价格如-1代表直接按按钮就能开</h1>
    <form action="setprice2" id="f1">
      配置密码
      <input type="text" id="testwebpassword" name="testwebpassword"><br>
      商品1价格<input type="text" id="price1" name="price1" value="0.00">元<br>
      商品2价格<input type="text" id="price2" name="price2" value="0.00">元<br>
      商品3价格<input type="text" id="price3" name="price3" value="0.00">元<br>
      商品4价格<input type="text" id="price4" name="price4" value="0.00">元<br>
      商品5价格<input type="text" id="price5" name="price5" value="0.00">元<br>
      商品6价格<input type="text" id="price6" name="price6" value="0.00">元<br>
      商品7价格<input type="text" id="price6" name="price7" value="0.00">元<br>
      商品8价格<input type="text" id="price6" name="price8" value="0.00">元<br>
      商品9价格<input type="text" id="price6" name="price9" value="0.00">元<br>
      商品10价格<input type="text" id="price6" name="price10" value="0.00">元<br>
      商品11价格<input type="text" id="price6" name="price11" value="0.00">元<br>
      商品12价格<input type="text" id="price6" name="price12" value="0.00">元<br>
      商品13价格<input type="text" id="price6" name="price13" value="0.00">元<br>
      商品14价格<input type="text" id="price6" name="price14" value="0.00">元<br>
      商品15价格<input type="text" id="price6" name="price15" value="0.00">元<br>
      商品16价格<input type="text" id="price6" name="price16" value="0.00">元<br>
      商品17价格<input type="text" id="price6" name="price17" value="0.00">元<br>
      商品18价格<input type="text" id="price6" name="price18" value="0.00">元<br>
    </form><button onclick="t()">提交更改</button>
    <!--<input value="提交">-->忘记密码，联系厂家<br><br><br><a href="/"><button>返回主页</button></a>
    <script>
        function t(){document.getElementById('f1').submit()}
    </script>
</body>

</html><!--)html";


const char *setdoorpin_html = R"html(<!DOCTYPE html>
<html>

<head>
    <meta charset="utf-8">
    <meta HTTP-EQUIV="pragma" CONTENT="no-cache"> 
    <title>设置价格页</title>
</head>

<body>
    <h1>门与电器引脚IO对应关系</h1>
    <form action="setdoorpin2" id="f1">
      配置密码
      <input type="text" id="testwebpassword" name="testwebpassword"><br>
      门1对应<input type="text" name="doorio1" value="1">号引脚IO<br>
      门2对应<input type="text" name="doorio2" value="2">号引脚IO<br>
      门3对应<input type="text" name="doorio3" value="3">号引脚IO<br>
      门4对应<input type="text" name="doorio4" value="4">号引脚IO<br>
      门5对应<input type="text" name="doorio5" value="5">号引脚IO<br>
      门6对应<input type="text" name="doorio6" value="6">号引脚IO<br>
      门7对应<input type="text" name="doorio7" value="7">号引脚IO<br>
      门8对应<input type="text" name="doorio8" value="8">号引脚IO<br>
      门9对应<input type="text" name="doorio9" value="9">号引脚IO<br>
      门10对应<input type="text" name="doorio10" value="10">号引脚IO<br>
      门11对应<input type="text" name="doorio11" value="11">号引脚IO<br>
      门12对应<input type="text" name="doorio12" value="12">号引脚IO<br>
      门13对应<input type="text" name="doorio13" value="13">号引脚IO<br>
      门14对应<input type="text" name="doorio14" value="18">号引脚IO<br>
      门15对应<input type="text" name="doorio15" value="19">号引脚IO<br>
      门16对应<input type="text" name="doorio16" value="20">号引脚IO<br>
      门17对应<input type="text" name="doorio17" value="21">号引脚IO<br>
      门18对应<input type="text" name="doorio18" value="0">号引脚IO<br>
      门开启时间<input type="text" name="dooropenms" value="2000">毫秒<br>
    </form><button onclick="t()">提交更改</button>
    <!--<input value="提交">-->忘记密码，联系厂家<br><br>
    ESP32-C3 9.9元开发板引脚非常用引脚对应关系
    RX0-----IO20影响串口调试<br>
    TX0-----IO21影响串口调试<br>
    IO12影响正面LED4<br>
    IO13影响正面LED5<br>
    IO18影响USB调试<br>
    IO19影响USB调试<br>
    IO9-----IO9-BOOT按钮影响信号<br>
    严禁使用IO14-IO15-IO16-IO17会影响程序运行<br>


    <br><a href="/"><button>返回主页</button></a>
    <script>
        function t(){document.getElementById('f1').submit()}
    </script>
</body>

</html><!--)html";

// const char* newpasswordhadchange = R"html(<!DOCTYPE html><html><head><meta charset="utf-8"></head><body>已成功更改为新密码,New password have changed</body></html><!--)html";
const char *alipayprivatekey_html = R"html(<!DOCTYPE html>
<html>

<head>
    <meta charset="utf-8">
    <title>设置支付宝私钥</title>
</head>

<body>
    <h1>设置支付宝私钥</h1>
    <form action="setalipayprivatekey2" method="GET" id="f1">支付宝PKCS#1格式私钥64个字符一个回车文件后缀常为pem<br><textarea name="newalipayprivatekey"
            rows="25" cols="38" placeholder="-----BEGIN RSA PRIVATE KEY-----
    MIIB……PKCS#1私钥64个字符一个回车
    -----END RSA PRIVATE KEY-----"></textarea>
    <br>支付宝根证书序列号alipay_root_cert_sn<input type="text" name="alipayrootcertsn" value="687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6" placeholder="687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6">
    <br>支付宝应用证书序列号app_cert_sn<input type="text" name="appcertsn">
    <br>支付宝项目号app_id<input type="text" name="appid">
    <br>各序列号需要用其他语言程序然后抓包获取,也可从官网获得SDK及DEMO
    <br>php 应用证书用 getCertSN()根证书用getRootCertSN()方法
    <br>密码<input type="text" name="testwebpassword">
    <br></form><button
        onclick="t()">提交更改</button><!--<input value="提交">-->忘记密码，联系厂家<br><br><br><a href="/"><button>返回主页</button></a>
    <script>function t() { document.getElementById('f1').submit() }</script>
</body>

</html><!--)html";

const char *alipaypublickey_html = R"html(<!DOCTYPE html>
<html>

<head>
    <meta charset="utf-8">
    <title>设置支付宝私钥</title>
</head>

<body>
    <h1>设置支付宝HTTPS公钥</h1><br>
    <h1>如果支付宝更换证书提供商需设置</h1>
    <form action="setalipaypublickeykey2" method="GET" id="f1">支付宝HTTPS公钥根证书，一般为DigiCert根证书2031年后需要更换平时切误设置<br>更换方法浏览器访问https://openapi.alipay.com/gateway.do点左上角小锁选更多选证书选最上面的根证书选导出仅根证书base64导出后用记事本打开并复制过来<BR><textarea name="newalipaypublickeykey"
            rows="25" cols="38">-----BEGIN CERTIFICATE-----
MIIDrzCCApegAwIBAgIQCDvgVpBCRrGhdWrJWZHHSjANBgkqhkiG9w0BAQUFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBD
QTAeFw0wNjExMTAwMDAwMDBaFw0zMTExMTAwMDAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IENBMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4jvhEXLeqKTTo1eqUKKPC3eQyaKl7hLOllsB
CSDMAZOnTjC3U/dDxGkAV53ijSLdhwZAAIEJzs4bg7/fzTtxRuLWZscFs3YnFo97
nh6Vfe63SKMI2tavegw5BmV/Sl0fvBf4q77uKNd0f3p4mVmFaG5cIzJLv07A6Fpt
43C/dxC//AH2hdmoRBBYMql1GNXRor5H4idq9Joz+EkIYIvUX7Q6hL+hqkpMfT7P
T19sdl6gSzeRntwi5m3OFBqOasv+zbMUZBfHWymeMr/y7vrTC0LUq7dBMtoM1O/4
gdW7jVg/tRvoSSiicNoxBN33shbyTApOB6jtSj1etX+jkMOvJwIDAQABo2MwYTAO
BgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUA95QNVbR
TLtm8KPiGxvDl7I90VUwHwYDVR0jBBgwFoAUA95QNVbRTLtm8KPiGxvDl7I90VUw
DQYJKoZIhvcNAQEFBQADggEBAMucN6pIExIK+t1EnE9SsPTfrgT1eXkIoyQY/Esr
hMAtudXH/vTBH1jLuG2cenTnmCmrEbXjcKChzUyImZOMkXDiqw8cvpOp/2PV5Adg
06O/nVsJ8dWO41P0jmP6P6fbtGbfYmbW0W5BjfIttep3Sp+dWOIrWcBAI+0tKIJF
PnlUkiaY4IBIqDfv8NZ5YBberOgOzW6sRBc4L0na4UU+Krk2U886UAb3LujEV0ls
YSEY1QSteDwsOoBrp+uvFRTp2InBuThs4pFsiv9kuXclVzDAGySj4dzp30d8tbQk
CAUw7C29C79Fv1C5qfPrmAESrciIxpg0X40KPMbp1ZWVbd4=
-----END CERTIFICATE-----</textarea>
    <br>密码<input type="text" name="testwebpassword">
    <br></form><button
        onclick="t()">提交更改</button><!--<input value="提交">-->忘记密码，联系厂家<br><br><br><a href="/"><button>返回主页</button></a>
    <script>function t() { document.getElementById('f1').submit() }</script>
</body>

</html><!--)html";
//b页面为直接调用页
const char *b_html = R"html(
<!DOCTYPE html>
<html>

<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>青岛迷之科技智能售货机</title>
  <script type="text/javascript" src="/qrcode.min.js"></script>
</head>

<body onload="buy()">

  <noscript>
    <p>
      <h1 style="color:red">
        使用本网站必须支持并启用JavaScript<br>如果您不启用将无法使用本网站<br>
      </h1>
    </p>
  </noscript>

    <h1 id="tip" style="font-size:3em;word-wrap: break-word;overflow: hidden;" >正在跳转支付,预计5秒</h1><br>
         
    <div id="demo"></div>
    <div id="qrcode" style="margin:1em;"></div>
   
</body>
 
<script>
  function buy() {
    var qrwidth;
    if(window.screen.height>=window.screen.width){qrwidth=window.screen.width*0.5;}
    if(window.screen.height<=window.screen.width){qrwidth=window.screen.height*0.5;}
    
    var qrcode = new QRCode(document.getElementById("qrcode"), {
        width : qrwidth,
        height : qrwidth
      });
      // qrcode.makeCode("214");

    var xhttpa = new XMLHttpRequest();
    var xhttpb = new XMLHttpRequest();
    var ask;
    var json;
    // var qrvalue;
    // var obj;
    xhttpa.onreadystatechange = function () {
      if (this.readyState == 4 && this.status == 200) {
        ask= this.responseText;
        var firstTwoChars = ask.substring(0, 4);
        if(firstTwoChars=="http"){
        xhttpb.open("GET", ask, true);//第二遍请求支付宝的网关
        xhttpb.send();
        }else{
          document.getElementById("tip").innerHTML ="出现问题"+ask;
        }
        
      }
    };
    xhttpb.onreadystatechange = function () {
      json= this.responseText;//支付宝返回的JSON字符串
      const obj = JSON.parse(json);
      if(obj.alipay_trade_precreate_response.code!=10000){document.getElementById("demo").innerHTML ="未请求成功:"+obj.alipay_trade_precreate_response.sub_msg+this.responseText;}
      else if(/AlipayClient/.test(window.navigator.userAgent)){
        window.location.replace(obj.alipay_trade_precreate_response.qr_code);
      }else{qrcode.makeCode(obj.alipay_trade_precreate_response.qr_code);
        document.getElementById("tip").innerHTML ="支付宝扫码完成支付";
      }
    }
    xhttpa.open("GET", "/buy?door="+GetQueryString("d"), true);//第一遍请求本地服务器索取请求Alipay的地址
    xhttpa.send();
  }

  function GetQueryString(name)
{
     var reg = new RegExp("(^|&)"+ name +"=([^&]*)(&|$)");
     var r = window.location.search.substr(1).match(reg);
     if(r!=null)return  unescape(r[2]); return null;
}
</script>
</html>
<!--
)html";



String rethtml(String str) {
  String ret;
  const char *head_html = R"html(<!DOCTYPE html><html><head><meta charset="utf-8"></head><body>)html";
  const char *feet_html = R"html(</body></html><!--)html";
  // char charMAX[str.length()+1]; // 做一个获取MAC地址的输出缓冲区
  // str.toCharArray(charMAX, sizeof(charMAX)); // 将MAC地址输出到char类型中
  ret = head_html;
  ret += str;
  ret += feet_html;
  // Serial.println(ret);
  return ret;
}


const char *qrcodeminjs = R"html(var QRCode;!function(){function a(a){this.mode=c.MODE_8BIT_BYTE,this.data=a,this.parsedData=[];for(var b=[],d=0,e=this.data.length;e>d;d++){var f=this.data.charCodeAt(d);f>65536?(b[0]=240|(1835008&f)>>>18,b[1]=128|(258048&f)>>>12,b[2]=128|(4032&f)>>>6,b[3]=128|63&f):f>2048?(b[0]=224|(61440&f)>>>12,b[1]=128|(4032&f)>>>6,b[2]=128|63&f):f>128?(b[0]=192|(1984&f)>>>6,b[1]=128|63&f):b[0]=f,this.parsedData=this.parsedData.concat(b)}this.parsedData.length!=this.data.length&&(this.parsedData.unshift(191),this.parsedData.unshift(187),this.parsedData.unshift(239))}function b(a,b){this.typeNumber=a,this.errorCorrectLevel=b,this.modules=null,this.moduleCount=0,this.dataCache=null,this.dataList=[]}function i(a,b){if(void 0==a.length)throw new Error(a.length+"/"+b);for(var c=0;c<a.length&&0==a[c];)c++;this.num=new Array(a.length-c+b);for(var d=0;d<a.length-c;d++)this.num[d]=a[d+c]}function j(a,b){this.totalCount=a,this.dataCount=b}function k(){this.buffer=[],this.length=0}function m(){return"undefined"!=typeof CanvasRenderingContext2D}function n(){var a=!1,b=navigator.userAgent;return/android/i.test(b)&&(a=!0,aMat=b.toString().match(/android ([0-9]\.[0-9])/i),aMat&&aMat[1]&&(a=parseFloat(aMat[1]))),a}function r(a,b){for(var c=1,e=s(a),f=0,g=l.length;g>=f;f++){var h=0;switch(b){case d.L:h=l[f][0];break;case d.M:h=l[f][1];break;case d.Q:h=l[f][2];break;case d.H:h=l[f][3]}if(h>=e)break;c++}if(c>l.length)throw new Error("Too long data");return c}function s(a){var b=encodeURI(a).toString().replace(/\%[0-9a-fA-F]{2}/g,"a");return b.length+(b.length!=a?3:0)}a.prototype={getLength:function(){return this.parsedData.length},write:function(a){for(var b=0,c=this.parsedData.length;c>b;b++)a.put(this.parsedData[b],8)}},b.prototype={addData:function(b){var c=new a(b);this.dataList.push(c),this.dataCache=null},isDark:function(a,b){if(0>a||this.moduleCount<=a||0>b||this.moduleCount<=b)throw new Error(a+","+b);return this.modules[a][b]},getModuleCount:function(){return this.moduleCount},make:function(){this.makeImpl(!1,this.getBestMaskPattern())},makeImpl:function(a,c){this.moduleCount=4*this.typeNumber+17,this.modules=new Array(this.moduleCount);for(var d=0;d<this.moduleCount;d++){this.modules[d]=new Array(this.moduleCount);for(var e=0;e<this.moduleCount;e++)this.modules[d][e]=null}this.setupPositionProbePattern(0,0),this.setupPositionProbePattern(this.moduleCount-7,0),this.setupPositionProbePattern(0,this.moduleCount-7),this.setupPositionAdjustPattern(),this.setupTimingPattern(),this.setupTypeInfo(a,c),this.typeNumber>=7&&this.setupTypeNumber(a),null==this.dataCache&&(this.dataCache=b.createData(this.typeNumber,this.errorCorrectLevel,this.dataList)),this.mapData(this.dataCache,c)},setupPositionProbePattern:function(a,b){for(var c=-1;7>=c;c++)if(!(-1>=a+c||this.moduleCount<=a+c))for(var d=-1;7>=d;d++)-1>=b+d||this.moduleCount<=b+d||(this.modules[a+c][b+d]=c>=0&&6>=c&&(0==d||6==d)||d>=0&&6>=d&&(0==c||6==c)||c>=2&&4>=c&&d>=2&&4>=d?!0:!1)},getBestMaskPattern:function(){for(var a=0,b=0,c=0;8>c;c++){this.makeImpl(!0,c);var d=f.getLostPoint(this);(0==c||a>d)&&(a=d,b=c)}return b},createMovieClip:function(a,b,c){var d=a.createEmptyMovieClip(b,c),e=1;this.make();for(var f=0;f<this.modules.length;f++)for(var g=f*e,h=0;h<this.modules[f].length;h++){var i=h*e,j=this.modules[f][h];j&&(d.beginFill(0,100),d.moveTo(i,g),d.lineTo(i+e,g),d.lineTo(i+e,g+e),d.lineTo(i,g+e),d.endFill())}return d},setupTimingPattern:function(){for(var a=8;a<this.moduleCount-8;a++)null==this.modules[a][6]&&(this.modules[a][6]=0==a%2);for(var b=8;b<this.moduleCount-8;b++)null==this.modules[6][b]&&(this.modules[6][b]=0==b%2)},setupPositionAdjustPattern:function(){for(var a=f.getPatternPosition(this.typeNumber),b=0;b<a.length;b++)for(var c=0;c<a.length;c++){var d=a[b],e=a[c];if(null==this.modules[d][e])for(var g=-2;2>=g;g++)for(var h=-2;2>=h;h++)this.modules[d+g][e+h]=-2==g||2==g||-2==h||2==h||0==g&&0==h?!0:!1}},setupTypeNumber:function(a){for(var b=f.getBCHTypeNumber(this.typeNumber),c=0;18>c;c++){var d=!a&&1==(1&b>>c);this.modules[Math.floor(c/3)][c%3+this.moduleCount-8-3]=d}for(var c=0;18>c;c++){var d=!a&&1==(1&b>>c);this.modules[c%3+this.moduleCount-8-3][Math.floor(c/3)]=d}},setupTypeInfo:function(a,b){for(var c=this.errorCorrectLevel<<3|b,d=f.getBCHTypeInfo(c),e=0;15>e;e++){var g=!a&&1==(1&d>>e);6>e?this.modules[e][8]=g:8>e?this.modules[e+1][8]=g:this.modules[this.moduleCount-15+e][8]=g}for(var e=0;15>e;e++){var g=!a&&1==(1&d>>e);8>e?this.modules[8][this.moduleCount-e-1]=g:9>e?this.modules[8][15-e-1+1]=g:this.modules[8][15-e-1]=g}this.modules[this.moduleCount-8][8]=!a},mapData:function(a,b){for(var c=-1,d=this.moduleCount-1,e=7,g=0,h=this.moduleCount-1;h>0;h-=2)for(6==h&&h--;;){for(var i=0;2>i;i++)if(null==this.modules[d][h-i]){var j=!1;g<a.length&&(j=1==(1&a[g]>>>e));var k=f.getMask(b,d,h-i);k&&(j=!j),this.modules[d][h-i]=j,e--,-1==e&&(g++,e=7)}if(d+=c,0>d||this.moduleCount<=d){d-=c,c=-c;break}}}},b.PAD0=236,b.PAD1=17,b.createData=function(a,c,d){for(var e=j.getRSBlocks(a,c),g=new k,h=0;h<d.length;h++){var i=d[h];g.put(i.mode,4),g.put(i.getLength(),f.getLengthInBits(i.mode,a)),i.write(g)}for(var l=0,h=0;h<e.length;h++)l+=e[h].dataCount;if(g.getLengthInBits()>8*l)throw new Error("code length overflow. ("+g.getLengthInBits()+">"+8*l+")");for(g.getLengthInBits()+4<=8*l&&g.put(0,4);0!=g.getLengthInBits()%8;)g.putBit(!1);for(;;){if(g.getLengthInBits()>=8*l)break;if(g.put(b.PAD0,8),g.getLengthInBits()>=8*l)break;g.put(b.PAD1,8)}return b.createBytes(g,e)},b.createBytes=function(a,b){for(var c=0,d=0,e=0,g=new Array(b.length),h=new Array(b.length),j=0;j<b.length;j++){var k=b[j].dataCount,l=b[j].totalCount-k;d=Math.max(d,k),e=Math.max(e,l),g[j]=new Array(k);for(var m=0;m<g[j].length;m++)g[j][m]=255&a.buffer[m+c];c+=k;var n=f.getErrorCorrectPolynomial(l),o=new i(g[j],n.getLength()-1),p=o.mod(n);h[j]=new Array(n.getLength()-1);for(var m=0;m<h[j].length;m++){var q=m+p.getLength()-h[j].length;h[j][m]=q>=0?p.get(q):0}}for(var r=0,m=0;m<b.length;m++)r+=b[m].totalCount;for(var s=new Array(r),t=0,m=0;d>m;m++)for(var j=0;j<b.length;j++)m<g[j].length&&(s[t++]=g[j][m]);for(var m=0;e>m;m++)for(var j=0;j<b.length;j++)m<h[j].length&&(s[t++]=h[j][m]);return s};for(var c={MODE_NUMBER:1,MODE_ALPHA_NUM:2,MODE_8BIT_BYTE:4,MODE_KANJI:8},d={L:1,M:0,Q:3,H:2},e={PATTERN000:0,PATTERN001:1,PATTERN010:2,PATTERN011:3,PATTERN100:4,PATTERN101:5,PATTERN110:6,PATTERN111:7},f={PATTERN_POSITION_TABLE:[[],[6,18],[6,22],[6,26],[6,30],[6,34],[6,22,38],[6,24,42],[6,26,46],[6,28,50],[6,30,54],[6,32,58],[6,34,62],[6,26,46,66],[6,26,48,70],[6,26,50,74],[6,30,54,78],[6,30,56,82],[6,30,58,86],[6,34,62,90],[6,28,50,72,94],[6,26,50,74,98],[6,30,54,78,102],[6,28,54,80,106],[6,32,58,84,110],[6,30,58,86,114],[6,34,62,90,118],[6,26,50,74,98,122],[6,30,54,78,102,126],[6,26,52,78,104,130],[6,30,56,82,108,134],[6,34,60,86,112,138],[6,30,58,86,114,142],[6,34,62,90,118,146],[6,30,54,78,102,126,150],[6,24,50,76,102,128,154],[6,28,54,80,106,132,158],[6,32,58,84,110,136,162],[6,26,54,82,110,138,166],[6,30,58,86,114,142,170]],G15:1335,G18:7973,G15_MASK:21522,getBCHTypeInfo:function(a){for(var b=a<<10;f.getBCHDigit(b)-f.getBCHDigit(f.G15)>=0;)b^=f.G15<<f.getBCHDigit(b)-f.getBCHDigit(f.G15);return(a<<10|b)^f.G15_MASK},getBCHTypeNumber:function(a){for(var b=a<<12;f.getBCHDigit(b)-f.getBCHDigit(f.G18)>=0;)b^=f.G18<<f.getBCHDigit(b)-f.getBCHDigit(f.G18);return a<<12|b},getBCHDigit:function(a){for(var b=0;0!=a;)b++,a>>>=1;return b},getPatternPosition:function(a){return f.PATTERN_POSITION_TABLE[a-1]},getMask:function(a,b,c){switch(a){case e.PATTERN000:return 0==(b+c)%2;case e.PATTERN001:return 0==b%2;case e.PATTERN010:return 0==c%3;case e.PATTERN011:return 0==(b+c)%3;case e.PATTERN100:return 0==(Math.floor(b/2)+Math.floor(c/3))%2;case e.PATTERN101:return 0==b*c%2+b*c%3;case e.PATTERN110:return 0==(b*c%2+b*c%3)%2;case e.PATTERN111:return 0==(b*c%3+(b+c)%2)%2;default:throw new Error("bad maskPattern:"+a)}},getErrorCorrectPolynomial:function(a){for(var b=new i([1],0),c=0;a>c;c++)b=b.multiply(new i([1,g.gexp(c)],0));return b},getLengthInBits:function(a,b){if(b>=1&&10>b)switch(a){case c.MODE_NUMBER:return 10;case c.MODE_ALPHA_NUM:return 9;case c.MODE_8BIT_BYTE:return 8;case c.MODE_KANJI:return 8;default:throw new Error("mode:"+a)}else if(27>b)switch(a){case c.MODE_NUMBER:return 12;case c.MODE_ALPHA_NUM:return 11;case c.MODE_8BIT_BYTE:return 16;case c.MODE_KANJI:return 10;default:throw new Error("mode:"+a)}else{if(!(41>b))throw new Error("type:"+b);switch(a){case c.MODE_NUMBER:return 14;case c.MODE_ALPHA_NUM:return 13;case c.MODE_8BIT_BYTE:return 16;case c.MODE_KANJI:return 12;default:throw new Error("mode:"+a)}}},getLostPoint:function(a){for(var b=a.getModuleCount(),c=0,d=0;b>d;d++)for(var e=0;b>e;e++){for(var f=0,g=a.isDark(d,e),h=-1;1>=h;h++)if(!(0>d+h||d+h>=b))for(var i=-1;1>=i;i++)0>e+i||e+i>=b||(0!=h||0!=i)&&g==a.isDark(d+h,e+i)&&f++;f>5&&(c+=3+f-5)}for(var d=0;b-1>d;d++)for(var e=0;b-1>e;e++){var j=0;a.isDark(d,e)&&j++,a.isDark(d+1,e)&&j++,a.isDark(d,e+1)&&j++,a.isDark(d+1,e+1)&&j++,(0==j||4==j)&&(c+=3)}for(var d=0;b>d;d++)for(var e=0;b-6>e;e++)a.isDark(d,e)&&!a.isDark(d,e+1)&&a.isDark(d,e+2)&&a.isDark(d,e+3)&&a.isDark(d,e+4)&&!a.isDark(d,e+5)&&a.isDark(d,e+6)&&(c+=40);for(var e=0;b>e;e++)for(var d=0;b-6>d;d++)a.isDark(d,e)&&!a.isDark(d+1,e)&&a.isDark(d+2,e)&&a.isDark(d+3,e)&&a.isDark(d+4,e)&&!a.isDark(d+5,e)&&a.isDark(d+6,e)&&(c+=40);for(var k=0,e=0;b>e;e++)for(var d=0;b>d;d++)a.isDark(d,e)&&k++;var l=Math.abs(100*k/b/b-50)/5;return c+=10*l}},g={glog:function(a){if(1>a)throw new Error("glog("+a+")");return g.LOG_TABLE[a]},gexp:function(a){for(;0>a;)a+=255;for(;a>=256;)a-=255;return g.EXP_TABLE[a]},EXP_TABLE:new Array(256),LOG_TABLE:new Array(256)},h=0;8>h;h++)g.EXP_TABLE[h]=1<<h;for(var h=8;256>h;h++)g.EXP_TABLE[h]=g.EXP_TABLE[h-4]^g.EXP_TABLE[h-5]^g.EXP_TABLE[h-6]^g.EXP_TABLE[h-8];for(var h=0;255>h;h++)g.LOG_TABLE[g.EXP_TABLE[h]]=h;i.prototype={get:function(a){return this.num[a]},getLength:function(){return this.num.length},multiply:function(a){for(var b=new Array(this.getLength()+a.getLength()-1),c=0;c<this.getLength();c++)for(var d=0;d<a.getLength();d++)b[c+d]^=g.gexp(g.glog(this.get(c))+g.glog(a.get(d)));return new i(b,0)},mod:function(a){if(this.getLength()-a.getLength()<0)return this;for(var b=g.glog(this.get(0))-g.glog(a.get(0)),c=new Array(this.getLength()),d=0;d<this.getLength();d++)c[d]=this.get(d);for(var d=0;d<a.getLength();d++)c[d]^=g.gexp(g.glog(a.get(d))+b);return new i(c,0).mod(a)}},j.RS_BLOCK_TABLE=[[1,26,19],[1,26,16],[1,26,13],[1,26,9],[1,44,34],[1,44,28],[1,44,22],[1,44,16],[1,70,55],[1,70,44],[2,35,17],[2,35,13],[1,100,80],[2,50,32],[2,50,24],[4,25,9],[1,134,108],[2,67,43],[2,33,15,2,34,16],[2,33,11,2,34,12],[2,86,68],[4,43,27],[4,43,19],[4,43,15],[2,98,78],[4,49,31],[2,32,14,4,33,15],[4,39,13,1,40,14],[2,121,97],[2,60,38,2,61,39],[4,40,18,2,41,19],[4,40,14,2,41,15],[2,146,116],[3,58,36,2,59,37],[4,36,16,4,37,17],[4,36,12,4,37,13],[2,86,68,2,87,69],[4,69,43,1,70,44],[6,43,19,2,44,20],[6,43,15,2,44,16],[4,101,81],[1,80,50,4,81,51],[4,50,22,4,51,23],[3,36,12,8,37,13],[2,116,92,2,117,93],[6,58,36,2,59,37],[4,46,20,6,47,21],[7,42,14,4,43,15],[4,133,107],[8,59,37,1,60,38],[8,44,20,4,45,21],[12,33,11,4,34,12],[3,145,115,1,146,116],[4,64,40,5,65,41],[11,36,16,5,37,17],[11,36,12,5,37,13],[5,109,87,1,110,88],[5,65,41,5,66,42],[5,54,24,7,55,25],[11,36,12],[5,122,98,1,123,99],[7,73,45,3,74,46],[15,43,19,2,44,20],[3,45,15,13,46,16],[1,135,107,5,136,108],[10,74,46,1,75,47],[1,50,22,15,51,23],[2,42,14,17,43,15],[5,150,120,1,151,121],[9,69,43,4,70,44],[17,50,22,1,51,23],[2,42,14,19,43,15],[3,141,113,4,142,114],[3,70,44,11,71,45],[17,47,21,4,48,22],[9,39,13,16,40,14],[3,135,107,5,136,108],[3,67,41,13,68,42],[15,54,24,5,55,25],[15,43,15,10,44,16],[4,144,116,4,145,117],[17,68,42],[17,50,22,6,51,23],[19,46,16,6,47,17],[2,139,111,7,140,112],[17,74,46],[7,54,24,16,55,25],[34,37,13],[4,151,121,5,152,122],[4,75,47,14,76,48],[11,54,24,14,55,25],[16,45,15,14,46,16],[6,147,117,4,148,118],[6,73,45,14,74,46],[11,54,24,16,55,25],[30,46,16,2,47,17],[8,132,106,4,133,107],[8,75,47,13,76,48],[7,54,24,22,55,25],[22,45,15,13,46,16],[10,142,114,2,143,115],[19,74,46,4,75,47],[28,50,22,6,51,23],[33,46,16,4,47,17],[8,152,122,4,153,123],[22,73,45,3,74,46],[8,53,23,26,54,24],[12,45,15,28,46,16],[3,147,117,10,148,118],[3,73,45,23,74,46],[4,54,24,31,55,25],[11,45,15,31,46,16],[7,146,116,7,147,117],[21,73,45,7,74,46],[1,53,23,37,54,24],[19,45,15,26,46,16],[5,145,115,10,146,116],[19,75,47,10,76,48],[15,54,24,25,55,25],[23,45,15,25,46,16],[13,145,115,3,146,116],[2,74,46,29,75,47],[42,54,24,1,55,25],[23,45,15,28,46,16],[17,145,115],[10,74,46,23,75,47],[10,54,24,35,55,25],[19,45,15,35,46,16],[17,145,115,1,146,116],[14,74,46,21,75,47],[29,54,24,19,55,25],[11,45,15,46,46,16],[13,145,115,6,146,116],[14,74,46,23,75,47],[44,54,24,7,55,25],[59,46,16,1,47,17],[12,151,121,7,152,122],[12,75,47,26,76,48],[39,54,24,14,55,25],[22,45,15,41,46,16],[6,151,121,14,152,122],[6,75,47,34,76,48],[46,54,24,10,55,25],[2,45,15,64,46,16],[17,152,122,4,153,123],[29,74,46,14,75,47],[49,54,24,10,55,25],[24,45,15,46,46,16],[4,152,122,18,153,123],[13,74,46,32,75,47],[48,54,24,14,55,25],[42,45,15,32,46,16],[20,147,117,4,148,118],[40,75,47,7,76,48],[43,54,24,22,55,25],[10,45,15,67,46,16],[19,148,118,6,149,119],[18,75,47,31,76,48],[34,54,24,34,55,25],[20,45,15,61,46,16]],j.getRSBlocks=function(a,b){var c=j.getRsBlockTable(a,b);if(void 0==c)throw new Error("bad rs block @ typeNumber:"+a+"/errorCorrectLevel:"+b);for(var d=c.length/3,e=[],f=0;d>f;f++)for(var g=c[3*f+0],h=c[3*f+1],i=c[3*f+2],k=0;g>k;k++)e.push(new j(h,i));return e},j.getRsBlockTable=function(a,b){switch(b){case d.L:return j.RS_BLOCK_TABLE[4*(a-1)+0];case d.M:return j.RS_BLOCK_TABLE[4*(a-1)+1];case d.Q:return j.RS_BLOCK_TABLE[4*(a-1)+2];case d.H:return j.RS_BLOCK_TABLE[4*(a-1)+3];default:return void 0}},k.prototype={get:function(a){var b=Math.floor(a/8);return 1==(1&this.buffer[b]>>>7-a%8)},put:function(a,b){for(var c=0;b>c;c++)this.putBit(1==(1&a>>>b-c-1))},getLengthInBits:function(){return this.length},putBit:function(a){var b=Math.floor(this.length/8);this.buffer.length<=b&&this.buffer.push(0),a&&(this.buffer[b]|=128>>>this.length%8),this.length++}};var l=[[17,14,11,7],[32,26,20,14],[53,42,32,24],[78,62,46,34],[106,84,60,44],[134,106,74,58],[154,122,86,64],[192,152,108,84],[230,180,130,98],[271,213,151,119],[321,251,177,137],[367,287,203,155],[425,331,241,177],[458,362,258,194],[520,412,292,220],[586,450,322,250],[644,504,364,280],[718,560,394,310],[792,624,442,338],[858,666,482,382],[929,711,509,403],[1003,779,565,439],[1091,857,611,461],[1171,911,661,511],[1273,997,715,535],[1367,1059,751,593],[1465,1125,805,625],[1528,1190,868,658],[1628,1264,908,698],[1732,1370,982,742],[1840,1452,1030,790],[1952,1538,1112,842],[2068,1628,1168,898],[2188,1722,1228,958],[2303,1809,1283,983],[2431,1911,1351,1051],[2563,1989,1423,1093],[2699,2099,1499,1139],[2809,2213,1579,1219],[2953,2331,1663,1273]],o=function(){var a=function(a,b){this._el=a,this._htOption=b};return a.prototype.draw=function(a){function g(a,b){var c=document.createElementNS("http://www.w3.org/2000/svg",a);for(var d in b)b.hasOwnProperty(d)&&c.setAttribute(d,b[d]);return c}var b=this._htOption,c=this._el,d=a.getModuleCount();Math.floor(b.width/d),Math.floor(b.height/d),this.clear();var h=g("svg",{viewBox:"0 0 "+String(d)+" "+String(d),width:"100%",height:"100%",fill:b.colorLight});h.setAttributeNS("http://www.w3.org/2000/xmlns/","xmlns:xlink","http://www.w3.org/1999/xlink"),c.appendChild(h),h.appendChild(g("rect",{fill:b.colorDark,width:"1",height:"1",id:"template"}));for(var i=0;d>i;i++)for(var j=0;d>j;j++)if(a.isDark(i,j)){var k=g("use",{x:String(i),y:String(j)});k.setAttributeNS("http://www.w3.org/1999/xlink","href","#template"),h.appendChild(k)}},a.prototype.clear=function(){for(;this._el.hasChildNodes();)this._el.removeChild(this._el.lastChild)},a}(),p="svg"===document.documentElement.tagName.toLowerCase(),q=p?o:m()?function(){function a(){this._elImage.src=this._elCanvas.toDataURL("image/png"),this._elImage.style.display="block",this._elCanvas.style.display="none"}function d(a,b){var c=this;if(c._fFail=b,c._fSuccess=a,null===c._bSupportDataURI){var d=document.createElement("img"),e=function(){c._bSupportDataURI=!1,c._fFail&&_fFail.call(c)},f=function(){c._bSupportDataURI=!0,c._fSuccess&&c._fSuccess.call(c)};return d.onabort=e,d.onerror=e,d.onload=f,d.src="data:image/gif;base64,iVBORw0KGgoAAAANSUhEUgAAAAUAAAAFCAYAAACNbyblAAAAHElEQVQI12P4//8/w38GIAXDIBKE0DHxgljNBAAO9TXL0Y4OHwAAAABJRU5ErkJggg==",void 0}c._bSupportDataURI===!0&&c._fSuccess?c._fSuccess.call(c):c._bSupportDataURI===!1&&c._fFail&&c._fFail.call(c)}if(this._android&&this._android<=2.1){var b=1/window.devicePixelRatio,c=CanvasRenderingContext2D.prototype.drawImage;CanvasRenderingContext2D.prototype.drawImage=function(a,d,e,f,g,h,i,j){if("nodeName"in a&&/img/i.test(a.nodeName))for(var l=arguments.length-1;l>=1;l--)arguments[l]=arguments[l]*b;else"undefined"==typeof j&&(arguments[1]*=b,arguments[2]*=b,arguments[3]*=b,arguments[4]*=b);c.apply(this,arguments)}}var e=function(a,b){this._bIsPainted=!1,this._android=n(),this._htOption=b,this._elCanvas=document.createElement("canvas"),this._elCanvas.width=b.width,this._elCanvas.height=b.height,a.appendChild(this._elCanvas),this._el=a,this._oContext=this._elCanvas.getContext("2d"),this._bIsPainted=!1,this._elImage=document.createElement("img"),this._elImage.style.display="none",this._el.appendChild(this._elImage),this._bSupportDataURI=null};return e.prototype.draw=function(a){var b=this._elImage,c=this._oContext,d=this._htOption,e=a.getModuleCount(),f=d.width/e,g=d.height/e,h=Math.round(f),i=Math.round(g);b.style.display="none",this.clear();for(var j=0;e>j;j++)for(var k=0;e>k;k++){var l=a.isDark(j,k),m=k*f,n=j*g;c.strokeStyle=l?d.colorDark:d.colorLight,c.lineWidth=1,c.fillStyle=l?d.colorDark:d.colorLight,c.fillRect(m,n,f,g),c.strokeRect(Math.floor(m)+.5,Math.floor(n)+.5,h,i),c.strokeRect(Math.ceil(m)-.5,Math.ceil(n)-.5,h,i)}this._bIsPainted=!0},e.prototype.makeImage=function(){this._bIsPainted&&d.call(this,a)},e.prototype.isPainted=function(){return this._bIsPainted},e.prototype.clear=function(){this._oContext.clearRect(0,0,this._elCanvas.width,this._elCanvas.height),this._bIsPainted=!1},e.prototype.round=function(a){return a?Math.floor(1e3*a)/1e3:a},e}():function(){var a=function(a,b){this._el=a,this._htOption=b};return a.prototype.draw=function(a){for(var b=this._htOption,c=this._el,d=a.getModuleCount(),e=Math.floor(b.width/d),f=Math.floor(b.height/d),g=['<table style="border:0;border-collapse:collapse;">'],h=0;d>h;h++){g.push("<tr>");for(var i=0;d>i;i++)g.push('<td style="border:0;border-collapse:collapse;padding:0;margin:0;width:'+e+"px;height:"+f+"px;background-color:"+(a.isDark(h,i)?b.colorDark:b.colorLight)+';"></td>');g.push("</tr>")}g.push("</table>"),c.innerHTML=g.join("");var j=c.childNodes[0],k=(b.width-j.offsetWidth)/2,l=(b.height-j.offsetHeight)/2;k>0&&l>0&&(j.style.margin=l+"px "+k+"px")},a.prototype.clear=function(){this._el.innerHTML=""},a}();QRCode=function(a,b){if(this._htOption={width:256,height:256,typeNumber:4,colorDark:"#000000",colorLight:"#ffffff",correctLevel:d.H},"string"==typeof b&&(b={text:b}),b)for(var c in b)this._htOption[c]=b[c];"string"==typeof a&&(a=document.getElementById(a)),this._android=n(),this._el=a,this._oQRCode=null,this._oDrawing=new q(this._el,this._htOption),this._htOption.text&&this.makeCode(this._htOption.text)},QRCode.prototype.makeCode=function(a){this._oQRCode=new b(r(a,this._htOption.correctLevel),this._htOption.correctLevel),this._oQRCode.addData(a),this._oQRCode.make(),this._el.title=a,this._oDrawing.draw(this._oQRCode),this.makeImage()},QRCode.prototype.makeImage=function(){"function"==typeof this._oDrawing.makeImage&&(!this._android||this._android>=3)&&this._oDrawing.makeImage()},QRCode.prototype.clear=function(){this._oDrawing.clear()},QRCode.CorrectLevel=d}();)html";
//清零重置密码计数器
void taskcleanwebpassworderrortime(void *pvParameters) {
  // pinMode(12, OUTPUT);

  while (1) {
    // digitalWrite(12, HIGH);
    // vTaskDelay(1000 / portTICK_PERIOD_MS);  // 闪烁频率1Hz
    // digitalWrite(12, LOW);
    // vTaskDelay(1000 / portTICK_PERIOD_MS);  // 闪烁频率1Hz
    if (webpassworderrortime > 0) { webpassworderrortime--; }
    if (alipayneedasktime > 0) {
      if (alipayneedasktime % 6 == 0) {
        // checkalipayorder();
      }

      if (alipayneedasktime == 1) {
        saveKeyValue("alipayneedasktime", String("0"));
      }
      alipayneedasktime--;
    }

    //反复轮询不停的预生产二维码请求字符串
    // prealipayf2fpay(needsigndoor);
    // Serial.println("测试预生产URL");
    // Serial.println("预生产URL");
    // Serial.println(needsigndoor);
    // needsigndoor++;
    // if(needsigndoor>=19){needsigndoor=1;}

    vTaskDelay(1000 / portTICK_PERIOD_MS);  // 闪烁频率1Hz
  }
}


// 保存键值对到文件
void saveKeyValue(const String &key, const String &value) {
  String filePath = "/" + key + ".txt";

  File file = LittleFS.open(filePath, "w");
  if (file) {
    file.print(value);
    file.close();
  } else {
    Serial.println("Failed to open file for writing");
  }
}

String getValueByKey(const String &key) {
  String filePath = "/" + key + ".txt";
  String value = "";

  File file = LittleFS.open(filePath, "r");
  if (file) {
    value = file.readString();
    file.close();
  } else {
    Serial.println("Failed to open file for reading");
  }

  return value;
}

void handleRoot(AsyncWebServerRequest *request) {  //主页代码拼接出注意事项然后写死
  String result;                                   //做一个2k的输出缓冲区
  String strMAC = WiFi.macAddress();               //获取MAC地址
  // char charMAC[18];                                //做一个获取MAC地址的输出缓冲区
  // strMAC.toCharArray(charMAC, sizeof(charMAC));    //将MAC地址输出到CHAR类型中
  result = String(index_html);
  result += strMAC;

  result += "</h1>";
  // <button>购买1号商品元</button>
  int i = 1;
  for (i; i < 19; i++) {
    String str = "price";
    str += String(i);
    if (getValueByKey(str).toDouble() != 0.00) {
      result += "<a href=\"b?d=";
      result += i;
      // result+= "&mac=";
      // result+=strMAC;
      result += "\"><button>购买";
      result += String(i);
      result += "号商品";
      if (getValueByKey(str).toDouble() >= 0.01) {
        result += getValueByKey(str);
      } else {
        result += String("0.00");
      }
      result += "元</button><br></a>";
    }
  }
  result += String(index_html2);
  request->send(200, "text/html", result);
}
void handleset(AsyncWebServerRequest *request) {
  // request->send(200, "text/plain", "LED state: " + state);
  request->send(200, "text/html", set_html);
}

void handlesetwebpassword(AsyncWebServerRequest *request) {
  // request->send(200, "text/plain", "LED state: " + state);


  request->send(200, "text/html", setwebpassword_html);
}
void handlesetwebpassword2(AsyncWebServerRequest *request) {
  Serial.println(websetpassword);
  if (webpassworderrortime) {
    //如果重试计时器未归零让客户等计时器
    char result[20];
    itoa(webpassworderrortime, result, 10);
    strcat(result, "sWait");
    request->send(200, "text/html", result);
    return;
  }
  String oldpassword = request->arg("oldpassword");
  String newpassword = request->arg("newpassword");
  if (newpassword.equals("")) {
    request->send(200, "text/html", rethtml("新密码不能为空"));
    return;
  }
  if (oldpassword.equals(websetpassword)) {

    //如果旧密码和储存的密码一致
    //则更新内存和硬盘中的密码
    saveKeyValue("websetpassword", newpassword);
    websetpassword = newpassword;
    request->send(200, "text/html", rethtml("已成功更改为新密码"));  //
    return;
  } else {
    webpassworderrortime = 15;
    Serial.println("输入的旧密码");
    Serial.println(oldpassword);
    Serial.println("设备内部的密码");
    Serial.println(websetpassword);
    request->send(200, "text/html", rethtml("旧密码核验不通过"));
  }
}

void handleControl(AsyncWebServerRequest *request) {
  String state = request->arg("state");

  if (state == "on") {
    digitalWrite(LED_PIN, HIGH);
  } else if (state == "off") {
    digitalWrite(LED_PIN, LOW);
  }
  // Serial.println(Sendalipay(getalipaytradeclosestr("202108230101010017b")));


  request->send(200, "text/plain", "LED state: " + state);
}
void handlesetalipayprivatekey(AsyncWebServerRequest *request) {
  request->send(200, "text/html", alipayprivatekey_html);
}
void handlesetalipayprivatekey2(AsyncWebServerRequest *request) {


  String newalipayprivatekey = request->arg("newalipayprivatekey");
  String testwebpassword = request->arg("testwebpassword");
  String alipayrootcertsnweb = request->arg("alipayrootcertsn");
  String appcertsnweb = request->arg("appcertsn");
  String appidweb = request->arg("appid");
  if (newalipayprivatekey.length() < 1) { request->send(400, "text/html", rethtml("私钥不能为空")); }
  if (testwebpassword.length() < 1) { request->send(400, "text/html", rethtml("密码不能为空")); }
  if (alipayrootcertsnweb.length() < 1) { request->send(400, "text/html", rethtml("支付宝根证书序列号不能为空")); }
  if (appcertsnweb.length() < 1) { request->send(400, "text/html", rethtml("应用证书序列号不能为空")); }
  if (appidweb.length() < 1) { request->send(400, "text/html", rethtml("APPID不能为空")); }


  //如果重试计时器未归零让客户等计时器
  if (webpassworderrortime) {

    char result[20];
    itoa(webpassworderrortime, result, 10);
    strcat(result, "sWait");
    request->send(200, "text/html", result);
    return;
  }
  //如果设备密码错误
  if (!testwebpassword.equals(websetpassword)) {
    webpassworderrortime = 15;
    request->send(200, "text/html", rethtml("旧密码核验不通过,Old password not right"));
    return;
  }
  //如果密码正确验证私钥合规性

  mbedtls_pk_context privatekey;  //建立一个结构体 privatekey 函数执行结束后该结构体会删除
  mbedtls_pk_init(&privatekey);
  // const char* charnewalipayprivatekey =newalipayprivatekey.c_str();//临时常量字符数组指针=创建一个char类型数据缓存为下一步填充密钥做准备
  char charArray[newalipayprivatekey.length() + 1];
  newalipayprivatekey.toCharArray(charArray, sizeof(charArray));


  int ret = mbedtls_pk_parse_key(&privatekey, (const unsigned char *)charArray, strlen(charArray) + 1, NULL, 0);
  if (ret != 0) {
    request->send(200, "text/html", rethtml("支付宝私钥不合格alipayprivatekey not right"));
    return;
  }
  saveKeyValue("alipayprivatekey", newalipayprivatekey);
  alipayprivatekey = newalipayprivatekey;
  saveKeyValue("alipayrootcertsn", alipayrootcertsnweb);
  alipayrootcertsn = alipayrootcertsnweb;
  saveKeyValue("appcertsn", appcertsnweb);
  alipayappcertsn = appcertsnweb;
  saveKeyValue("appid", appidweb);
  alipayappid = appidweb;
  request->send(200, "text/html", rethtml("支付宝参数已存储 alipay parameter saved"));
  return;
}
//设置支付宝HTTPS公钥
void handlesetalipaypublickey(AsyncWebServerRequest *request) {
  request->send(200, "text/html", alipaypublickey_html);
}
void handlesetalipaypublickey2(AsyncWebServerRequest *request) {


  String newalipaypublickeykey = request->arg("newalipaypublickeykey");
  String testwebpassword = request->arg("testwebpassword");
  if (newalipaypublickeykey.length() < 1) { request->send(400, "text/html", rethtml("私钥不能为空")); }
  if (testwebpassword.length() < 1) { request->send(400, "text/html", rethtml("密码不能为空")); }


  //如果重试计时器未归零让客户等计时器
  if (webpassworderrortime) {

    char result[20];
    itoa(webpassworderrortime, result, 10);
    strcat(result, "sWait");
    request->send(200, "text/html", result);
    return;
  }
  //如果设备密码错误
  if (!testwebpassword.equals(websetpassword)) {
    webpassworderrortime = 15;
    request->send(200, "text/html", rethtml("旧密码核验不通过,Old password not right"));
    return;
  }
  //如果密码正确验证私钥合规性

  // mbedtls_pk_context privatekey;  //建立一个结构体 privatekey 函数执行结束后该结构体会删除
  // mbedtls_pk_init(&privatekey);
  // // const char* charnewalipayprivatekey =newalipayprivatekey.c_str();//临时常量字符数组指针=创建一个char类型数据缓存为下一步填充密钥做准备
  // char charArray[newalipayprivatekey.length() + 1];
  // newalipayprivatekey.toCharArray(charArray, sizeof(charArray));


  // int ret = mbedtls_pk_parse_key(&privatekey, (const unsigned char *)charArray, strlen(charArray) + 1, NULL, 0);
  // if (ret != 0) {
  //   request->send(200, "text/html", rethtml("支付宝私钥不合格alipayprivatekey not right"));
  //   return;
  // }
  saveKeyValue("alipayhttpspublickeykey", newalipaypublickeykey);
  request->send(200, "text/html", rethtml("支付宝https公钥已存储 alipay https publickeykey saved"));
  return;
}
//直调跳转 一个门上一个码扫了付钱就走
void handleb(AsyncWebServerRequest *request) {
  request->send(200, "text/html", b_html);
}
void handlesetqrcodeminjs(AsyncWebServerRequest *request) {
  request->send(200, "text/js", qrcodeminjs);
}
void handlesetprice(AsyncWebServerRequest *request) {
  request->send(200, "text/html", setprice_html);
}
void handlesetprice2(AsyncWebServerRequest *request) {
  String testwebpassword = request->arg("testwebpassword");
  String price1 = String(request->arg("price1").toDouble(), 2);
  String price2 = String(request->arg("price2").toDouble(), 2);
  String price3 = String(request->arg("price3").toDouble(), 2);
  String price4 = String(request->arg("price4").toDouble(), 2);
  String price5 = String(request->arg("price5").toDouble(), 2);
  String price6 = String(request->arg("price6").toDouble(), 2);
  String price7 = String(request->arg("price7").toDouble(), 2);
  String price8 = String(request->arg("price8").toDouble(), 2);
  String price9 = String(request->arg("price9").toDouble(), 2);
  String price10 = String(request->arg("price10").toDouble(), 2);
  String price11 = String(request->arg("price11").toDouble(), 2);
  String price12 = String(request->arg("price12").toDouble(), 2);
  String price13 = String(request->arg("price13").toDouble(), 2);
  String price14 = String(request->arg("price14").toDouble(), 2);
  String price15 = String(request->arg("price15").toDouble(), 2);
  String price16 = String(request->arg("price16").toDouble(), 2);
  String price17 = String(request->arg("price17").toDouble(), 2);
  String price18 = String(request->arg("price18").toDouble(), 2);
  //如果重试计时器未归零让客户等计时器
  if (webpassworderrortime) {

    char result[20];
    itoa(webpassworderrortime, result, 10);
    strcat(result, "sWait");
    request->send(200, "text/html", result);
    return;
  }
  //如果设备密码错误
  if (!testwebpassword.equals(websetpassword)) {
    webpassworderrortime = 15;
    request->send(200, "text/html", rethtml("旧密码核验不通过,Old password not right"));
    return;
  }
  //如果密码正确直接存库
  price[1] = price1;
  price[2] = price2;
  price[3] = price3;
  price[4] = price4;
  price[5] = price5;
  price[6] = price6;
  price[7] = price7;
  price[8] = price8;
  price[9] = price9;
  price[10] = price10;
  price[11] = price11;
  price[12] = price12;
  price[13] = price13;
  price[14] = price14;
  price[15] = price15;
  price[16] = price16;
  price[17] = price17;
  price[18] = price18;

  saveKeyValue("price1", price1);
  saveKeyValue("price2", price2);
  saveKeyValue("price3", price3);
  saveKeyValue("price4", price4);
  saveKeyValue("price5", price5);
  saveKeyValue("price6", price6);
  saveKeyValue("price7", price7);
  saveKeyValue("price8", price8);
  saveKeyValue("price9", price9);
  saveKeyValue("price10", price10);
  saveKeyValue("price11", price11);
  saveKeyValue("price12", price12);
  saveKeyValue("price13", price13);
  saveKeyValue("price14", price14);
  saveKeyValue("price15", price15);
  saveKeyValue("price16", price16);
  saveKeyValue("price17", price17);
  saveKeyValue("price18", price18);
  request->send(200, "text/html", rethtml("价格已存储 price saved"));
  return;
}

//通过HTTPS获得时间
bool HTTPS_GETTIME(String host, String postRequest, int Port = 443, int Receive_cache = 1024) {
  WiFiClientSecure HTTPS;  //建立WiFiClientSecure对象
  HTTPS.setCACert(rootCACertificate);
  postRequest = (String)("GET ") + postRequest + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "User-Agent: ESP32WiFiClientSecure" + "\r\n\r\n";
  HTTPS.setInsecure();  //不进行服务器身份认证
  int cache = sizeof(postRequest) + 10;
  // Serial.print("原始URLB");
  // Serial.println(postRequest);
  // Serial.print("发送缓存：");
  // Serial.println(postRequest);
  // Serial.print("发送缓存大小：");
  // Serial.println(cache);
  // HTTPS.setBufferSizes(Receive_cache, cache); //接收和发送缓存大小
  HTTPS.setTimeout(15000);  //设置等待的最大毫秒数
  // Serial.println("初始化参数完毕！\n开始连接服务器==>>>>>");
  if (!HTTPS.connect(host.c_str(), Port)) {
    // delay(100);
    // Serial.println();
    Serial.println("服务器连接失败！");
    return false;
  } else {
    Serial.println("服务器连接成功！\r");
    // Serial.println("发送请求：\n" + postRequest);
  }
  HTTPS.print(postRequest.c_str());  // 发送HTTP请求

  // 检查服务器响应信息。通过串口监视器输出服务器状态码和响应头信息
  // 从而确定ESP8266已经成功连接服务器
  Serial.println("获取响应信息========>：\r");
  Serial.println("响应头：");
  String inputString;  //用一个字符串接收时间变量
  int i = 0;
  while (HTTPS.connected()) {
    inputString = HTTPS.readStringUntil('\n');
    if (inputString.startsWith("Date: ")) {
      Serial.println(inputString);  //如果是Date开头的字符串就截取
      break;
    }
    if (inputString == "\r") {
      Serial.println("响应头输出完毕！");  // Serial.println("响应头屏蔽完毕！\r");
      break;
    }

    if (i >= 20) {
      break;  //防while(1)
    }
    i++;
  }

  char *tokens[6];  // 用于存储分割后的子字符串
  int tokenCount = 0;

  char *str = strdup(inputString.c_str());  // 复制输入字符串，并将其转换为字符数组

  char *token = strtok(str, " ");  // 使用空格作为分隔符

  while (token != NULL && tokenCount < 6) {
    tokens[tokenCount] = token;  // 存储子字符串
    tokenCount++;

    token = strtok(NULL, " ");  // 继续分割下一个子字符串
  }

  // 检查是否成功分割字符串
  if (tokenCount != 6) { return false; }
  // 分隔后的子字符串数组
  String year = String(tokens[4]);
  String month = String(tokens[3]);
  String day = String(tokens[2]);
  String time = String(tokens[5]);

  // 进一步处理截取到的子字符串
  Serial.println("Year: " + year);
  Serial.println("Month: " + month);
  Serial.println("Day: " + day);

  // 分割时间部分，使用冒号作为分隔符
  char *timeTokens[3];  // 用于存储分割后的时间子字符串
  int timeTokenCount = 0;

  char *timeStr = strdup(time.c_str());  // 复制时间字符串，并将其转换为字符数组

  char *timeToken = strtok(timeStr, ":");  // 使用冒号作为分隔符

  while (timeToken != NULL && timeTokenCount < 3) {
    timeTokens[timeTokenCount] = timeToken;  // 存储时间子字符串
    timeTokenCount++;

    timeToken = strtok(NULL, ":");  // 继续分割下一个时间子字符串
  }

  if (timeTokenCount != 3) { return false; }
  String hour = String(timeTokens[0]);
  String minute = String(timeTokens[1]);
  String second = String(timeTokens[2]);

  Serial.println("Hour: " + hour);
  Serial.println("Minute: " + minute);
  Serial.println("Second: " + second);


  free(timeStr);  // 释放分配的内存


  free(str);  // 释放分配的内存
  //此时已分割好所有字符串
  //对月份进行转换 先做一个对象数组
  // String monthAbbreviation = String(month);
  String monthAbbreviations[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };

  int monthNumbers[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
  };

  int monthNumber = -1;  // 初始化月份数字

  // 在数组中查找月份缩写，并获取对应的月份数字
  for (int i = 0; i < sizeof(monthAbbreviations) / sizeof(monthAbbreviations[0]); i++) {
    if (month == monthAbbreviations[i]) {
      monthNumber = monthNumbers[i];
      break;
    }
  }
  if (monthNumber == -1) { return false; }  //如果月份不存在则返回报错
  // 输出转换后的结果
  // if (monthNumber != -1) {
  //   Serial.println("Month number: " + String(monthNumber));
  // } else {
  //   Serial.println("Invalid input");
  // }

  // monthNumber = monthMap[month];

  //此时可以使用Arduino自带的setTime(int hr,int min,int sec,int dy, int mnth, int yr) 但该函数存在32位年溢出问题
  tmElement64s_t tm;
  tm.Year = strtoul(year.c_str(), nullptr, 10) - 1970;
  tm.Month = monthNumber;
  tm.Day = strtoul(day.c_str(), nullptr, 10);
  tm.Hour = strtoul(hour.c_str(), nullptr, 10);
  tm.Minute = strtoul(minute.c_str(), nullptr, 10);
  tm.Second = strtoul(second.c_str(), nullptr, 10);
  ntpTime = makeTime(tm);  //将时间戳赋值给全局变量ntpTime;
  Serial.println("授时时间戳901");
  Serial.println(ntpTime);

  struct timeval tv;  //ESP32核心库定义此结构体用于赋值ESP32内置RTC
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  Serial.println("执行授时908");
  Serial.println(ntpTime);
  needupadte = 0;
  return true;
}


//购买处理页句柄
//先根据价格判断是否需要调用支付宝
//如果价格<0 直接开门，如果价格大于0则调用支付宝，拿到返回的跳转地址后交由前端，并启动后端轮询进行轮询5小时
void handlebuy(AsyncWebServerRequest *request) {

  int door = request->arg("door").toInt();  //过滤非正常输入
  if (door >= 19) { request->send(200, "text/html", rethtml("没有那么多门")); }
  if (door <= 0) { request->send(200, "text/html", rethtml("门号错误，门号小于0")); }
  String str = "price";
  str += String(door);
  String pricestr = price[door];
  double price = pricestr.toDouble();  //读取全局变量提速
  if (price <= -0.01) {
    request->send(200, "text/html", rethtml("门已开"));
    pinMode(13, OUTPUT);
    digitalWrite(13, HIGH);
    delay(2000);
    digitalWrite(13, LOW);
  }
  //授时
  if (needupadte) {
    HTTPS_GETTIME("openapi.alipay.com", "/gateway.do?method=alipay.trade.query", 443, 1024);
    // Serial.println("尝试HTTP授时");
  }
  // if (needupadte) {
  //   getNtpTimeAsync();
  //   delay(3000);
  //   Serial.println("等待授时");
  // }
  //如果无法连接支付宝服务器获得授时
  // if (HTTPS_GETTIME("openapi.alipay.com", "/gateway.do?method=alipay.trade.query", 443, 1024)) {

  // } else {
  //   request->send(200, "text/html", rethtml("无法得到授时"));
  // }
  //   Serial.println("999");
  // Serial.println(micros());
  // request->send(200, "text/html", rethtml("无法得到授时"));
  //如果价格大于0.01就调用支付宝
  if (needupadte) {

    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", rethtml("无法得到授时"));
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  }
  if (price >= 0.01) {
    //网关地址
    String url = "https://openapi.alipay.com/gateway.do?";  // Replace with your URL
    //检测前述订单并关闭前述订单，如果有就调用预生产的二维码，如果没有就立刻生成二维码
    // url += getalipayparam(price, door);//如果内存不足就每来一个客户现申请
    if (alipaysigntime[door] == alipaydealtime) {
      Serial.println("使用预存");
      url += regetprealipayf2fpay(door);
    } else {
      Serial.println("非预存");
      url += getalipayparam(price, door);
    }
    alipaydealtime++;  //alipaydealtime指的是下一次需要生成的订单号结尾

    struct timeval tv;  //创造一个结构体用于重置内部RTC
    gettimeofday(&tv, NULL);
    tmElement64s_t tm;
    alipayneedquerytrytime = ntpTime + tv.tv_sec + 330;  //轮询时间限制时间戳
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", url);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    saveKeyValue("alipaydealtime", String(alipaydealtime));                  //
    saveKeyValue("alipayneedquerytrytime", String(alipayneedquerytrytime));  //保存轮询时间戳到硬盘
    alipayneedquerytrytime = alipayneedquerytrytime;                         //保存时间戳到硬盘

    //根据本次的alipaydealtime和门号计算出本次全部订单号并保存至全局变量
    String doorstr = String(door);
    if (doorstr.length() < 2) {
      doorstr = "0" + doorstr;
    }
    String strMAC = WiFi.macAddress();
    strMAC.replace(":", "");
    // String param = "alipay_root_cert_sn=";
    String outtradenonow = String("mizhikeji.com") + doorstr + strMAC + String(alipaydealtime - 1);  // alipaydealtime为全局变量 在调用此函数时应自加1
    Serial.println("当前订单号");
    // Serial.println(outtradenonow);
    //查询订单轮询结束时间戳
    alipayneedqueryorder = outtradenonow;  //当前需要轮询的订单号
    saveKeyValue("alipayneedqueryorder", String(outtradenonow));
    //此时alipaydealtime计数器增加了但之前的订单是否支付成为了问题
    //轮询队列的订单号为alipaydealtime-1 预生成订单号为 alipaydealtime
    //由于后人竟拍需要查询并关闭前人的购买 订单号为alipaydealtime-2
    //需要注意的是，如果服务器返回超时会造成句柄看门狗触发导致重启系统


    if (alipaydealtime >= 2) {
      //先计算订单号
      doorstr = String(alipaylastdoor);
      if (doorstr.length() < 2) {
        doorstr = "0" + doorstr;
      }
      // String strMAC = WiFi.macAddress();
      // strMAC.replace(":", "");
      // String param = "alipay_root_cert_sn=";
      String outtradeno = String("mizhikeji.com") + doorstr + strMAC + String(alipaydealtime - 2);  // alipaydealtime为全局变量 在调用此函数时应自加1
      Serial.println("上一订单号");
      Serial.println(outtradeno);
      alipayneedcloseorder = outtradeno;  //保存全局变量
      saveKeyValue("alipayneedcloseorder", String(alipayneedcloseorder));
      alipayneedclosetrytime = ntpTime + tv.tv_sec + 330;                      //设置轮询截至时间5分30秒
      saveKeyValue("alipayneedclosetrytime", String(alipayneedclosetrytime));  //保存轮询时间


      // Serial.println("尝试关闭订单");
      // Serial.println(Sendalipay(getalipaytradeclosestr(outtradeno)));
      // Serial.println("尝试查询订单");
      // checkalipayorderbyordernumber(outtradeno);
      // checkalipayorderbyalipaydealtime(alipaydealtime - 2, alipaylastdoor);  //此时存储在全局变量中的lastdoor并未更换
      //将关键参数提交轮询算法线程避免在此函数执行过多时间
      //因为支付宝订单预创建接口不能提前关闭，所以要在订单生命周期不停的让支付宝关闭订单
      // xTaskCreate(taskcheckandclose, outtradeno.c_str(), 10000, (void *)&outtradeno, 1, NULL);
    }

    alipaylastdoor = door;  //全局变量赋值
    saveKeyValue("alipaylastdoor", String(door));
    //     saveKeyValue("lastorder", outtradeno);
    // saveKeyValue("lastdoor", doorstr);
    // saveKeyValue("alipayneedasktime", String("300"));
    // request->send(200, "text/html", url);
    // esp_task_wdt_reset();
    //检测前述订单后发完请求后尝试关闭之前的订单
    // Sendalipay(getalipaytradeclosestr(lastorder));
    // String alpayjson=Sendalipay(getalipaytradequerystr(lastorder));
    //   if (resolutionAlipaytradequeryrespon(alpayjson)) {
    //     Serial.println(alpayjson);
    //   }
  } else {
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", rethtml("该商品未启用"));
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    // request->send(200, "text/html", rethtml("该商品未启用"));
  }
}
//创建一个线程疯狂轮询关闭订单
void taskcheckandclose(void *parameter) {
  String strParam = *((String *)parameter);
  int i = 61;
  while (i--) {
    Serial.println("尝试关闭订单");
    //Serial.println(Sendalipay(getalipaytradeclosestr(strParam)));
    Serial.println("尝试查询订单");
    checkalipayorderbyordernumber(strParam);
    vTaskDelay(pdMS_TO_TICKS(5000));  // 延时5秒
  }
}

void handlesetdoorpin(AsyncWebServerRequest *request) {
  request->send(200, "text/html", setdoorpin_html);
}

void handlesetdoorpin2(AsyncWebServerRequest *request) {
  String testwebpassword = request->arg("testwebpassword");
  int door = request->arg("door").toInt();  //过滤非正常输入
  String doorio1 = String(request->arg("doorio1").toInt());
  String doorio2 = String(request->arg("doorio2").toInt());
  String doorio3 = String(request->arg("doorio3").toInt());
  String doorio4 = String(request->arg("doorio4").toInt());
  String doorio5 = String(request->arg("doorio5").toInt());
  String doorio6 = String(request->arg("doorio6").toInt());
  String doorio7 = String(request->arg("doorio7").toInt());
  String doorio8 = String(request->arg("doorio8").toInt());
  String doorio9 = String(request->arg("doorio9").toInt());
  String doorio10 = String(request->arg("doorio10").toInt());
  String doorio11 = String(request->arg("doorio11").toInt());
  String doorio12 = String(request->arg("doorio12").toInt());
  String doorio13 = String(request->arg("doorio13").toInt());
  String doorio14 = String(request->arg("doorio14").toInt());
  String doorio15 = String(request->arg("doorio15").toInt());
  String doorio16 = String(request->arg("doorio16").toInt());
  String doorio17 = String(request->arg("doorio17").toInt());
  String doorio18 = String(request->arg("doorio18").toInt());
  String dooropenms = String(request->arg("dooropenms").toInt());
  //如果重试计时器未归零让客户等计时器
  if (webpassworderrortime) {

    char result[20];
    itoa(webpassworderrortime, result, 10);
    strcat(result, "sWait");
    request->send(200, "text/html", result);
    return;
  }
  //如果设备密码错误
  if (!testwebpassword.equals(websetpassword)) {
    webpassworderrortime = 15;
    request->send(200, "text/html", rethtml("旧密码核验不通过,Old password not right"));
    return;
  }
  //如果密码正确直接存库

  doorio[1] = doorio1.toInt();
  doorio[2] = doorio2.toInt();
  doorio[3] = doorio3.toInt();
  doorio[4] = doorio4.toInt();
  doorio[5] = doorio5.toInt();
  doorio[6] = doorio6.toInt();
  doorio[7] = doorio7.toInt();
  doorio[8] = doorio8.toInt();
  doorio[9] = doorio9.toInt();
  doorio[10] = doorio10.toInt();
  doorio[11] = doorio11.toInt();
  doorio[12] = doorio12.toInt();
  doorio[13] = doorio13.toInt();
  doorio[14] = doorio14.toInt();
  doorio[15] = doorio15.toInt();
  doorio[16] = doorio16.toInt();
  doorio[17] = doorio17.toInt();
  doorio[18] = doorio18.toInt();


  saveKeyValue("doorio1", doorio1);
  saveKeyValue("doorio2", doorio2);
  saveKeyValue("doorio3", doorio3);
  saveKeyValue("doorio4", doorio4);
  saveKeyValue("doorio5", doorio5);
  saveKeyValue("doorio6", doorio6);
  saveKeyValue("doorio7", doorio7);
  saveKeyValue("doorio8", doorio8);
  saveKeyValue("doorio9", doorio9);
  saveKeyValue("doorio10", doorio10);
  saveKeyValue("doorio11", doorio11);
  saveKeyValue("doorio12", doorio12);
  saveKeyValue("doorio13", doorio13);
  saveKeyValue("doorio14", doorio14);
  saveKeyValue("doorio15", doorio15);
  saveKeyValue("doorio16", doorio16);
  saveKeyValue("doorio17", doorio17);
  saveKeyValue("doorio18", doorio18);
  saveKeyValue("dooropenms", dooropenms);
  request->send(200, "text/html", rethtml("门IO配置已存储 doorio saved"));
  return;
}

void handleecho(AsyncWebServerRequest *request) {
  // String state = request->arg("state");

  // if (state == "on") {
  //   digitalWrite(LED_PIN, HIGH);
  // } else if (state == "off") {
  //   digitalWrite(LED_PIN, LOW);
  // }
  i++;
  request->send(200, "text/plain", "open time: " + (String)i);
}
//关键函数 对支付宝请求执行签名 签名算法RSA2(SHA256withRSA)正式名称RSASSA- PKCS1-v1_5实现PKCS#1 v1.5填充和模幂运算
//请使用过时RSA1(SHA1withRSA)算法及MD5算法的朋友及时升级
//个人实力有限实在不能调用ESP32-C3中的硬件加速器。基于软件实现RSA（没有中间层很难去补底层）
String alipaysign(String alipaydata) {
  // 需要包含以下库
  // #include <mbedtls/pk.h>
  // #include <mbedtls/rsa.h>
  // #include <mbedtls/pem.h>
  // #include <mbedtls/sha256.h>
  // #include <mbedtls/ctr_drbg.h>
  // #include <mbedtls/entropy.h>//https://johanneskinzig.de/index.php/files/26/Arduino-mbedtls/9/gettingstartedmbedtlsarduino.7z
  // #include <arduino_base64.hpp>//https://github.com/dojyorin/arduino_base64 随便拉的库 库管理搜base64_encode作者dojyorin
  //关键私钥从LITTLE FS中获得 由于私钥太占内存所以我将其写至签名函数内，避免继续在上层函数中继续占用内存
  //私钥 这里通过读取littleFS从SPIflash中读取
  //存在重大漏洞 如果有人取下SPIflash做逆向工程可以读出您的私钥伪造您的签名使用支付宝转账接口盗取您的支付宝资金

  //直接调用全局变量
  // String alipayprivatekey = getValueByKey("alipayprivatekey");  //从Little存储中的简易键值存储中读取私钥
  const char *data = alipaydata.c_str();

  const char *pemKey = alipayprivatekey.c_str();
  mbedtls_pk_context key;
  mbedtls_rsa_context *rsa = NULL;  //实际未利用子RSA函数库中的签名算法
  mbedtls_ctr_drbg_context ctr_drbg;

  mbedtls_pk_init(&key);
  mbedtls_ctr_drbg_init(&ctr_drbg);

  const char *personalization = "random_seed142";  //随机数种子 签名计算不牵扯随机数生成器 无需设置根据真实事件产生的随机数种子
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char *)personalization, strlen(personalization));
  //载入RSA密钥
  int ret = mbedtls_pk_parse_key(&key, (const unsigned char *)pemKey, strlen(pemKey) + 1, NULL, 0);
  if (ret != 0) {
    Serial.println("Failed to parse private key.");
    Serial.println(ret);
    return "";
  }
  // 计算SHA256哈希值
  unsigned char hash[32];
  mbedtls_sha256((const unsigned char *)data, strlen(data), hash, 0);
  // 使用密钥对哈希值进行数字签名
  unsigned char signature[256];
  size_t signatureLen;
  ret = mbedtls_pk_sign(&key, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, &signatureLen, mbedtls_ctr_drbg_random, &ctr_drbg);
  if (ret != 0) {
    Serial.println("Failed to sign the data.");
    return "";
  }
  char base64signoutput[base64::encodeLength(sizeof(signature))];  //制造一个长度仅为base生成长度一致的字符串
  base64::encode(signature, sizeof(signature), base64signoutput);  //把字符串base64算法存入字符数组base64signoutput
  // 清理资源
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&ctr_drbg);
  mbedtls_entropy_free(&entropy);
  String strbase64signoutput;
  strbase64signoutput = base64signoutput;
  return strbase64signoutput;
}
//通过NTP进行授时
void getNtpTimeAsync() {  //发UDP包请求时间后调用句柄监听

  IPAddress ntpIpAddress;
  byte packetBuffer[48];
  packetBuffer[0] = 0b00101011;  // LI, Version, Mode 不闰秒 ntpv5 客户端
  packetBuffer[1] = 0;           // Stratum, or type of clock时钟精度 1最高15最低
  packetBuffer[2] = 6;
  packetBuffer[3] = 0x00;
  if (WiFi.hostByName(ntpServer, ntpIpAddress)) {


    udp.writeTo(packetBuffer, 48, ntpIpAddress, 123);
    timesendudp = micros();
  } else {
    // Serial.println("Failed to resolve NTP server");
  }
}
//回调后调用句柄
void handleNtpResponse(AsyncUDPPacket packet) {
  int packlength = packet.length();

  if (packlength != 0) {
    timegetudp = micros();
    if (timegetudp < timesendudp) { return; }
    if ((timegetudp - timesendudp) > 3000000) { return; }  //如果对时包返回的过晚则代表网络拥塞此次对时失败
    byte packetBuffer[packlength];
    packet.read(packetBuffer, packlength);
    // Serial.println("当前服务器");
    // Serial.println(ntpServer);
    unsigned long bite1 = word(0x00, packetBuffer[0]);
    bite1 = bite1 & 0x38;
    bite1 = bite1 >> 3;
    if (bite1 != 5) { return; }  //如果对时版本号不对则代表服务器不支持ntp5
    // Serial.println("版本号");
    // Serial.println(bite1);
    // Serial.println("原始数据");
    // for (size_t i = 0; i < packlength; i++) {
    // if(i%4==0){Serial.println();}
    // Serial.printf("%02x",packetBuffer[i]);
    // //Serial.print(packetBuffer[i], HEX);

    // Serial.print(" ");
    // }

    // 提取时间戳字段

    unsigned Era = packetBuffer[5];
    unsigned ntpgetTimehighWord = word(packetBuffer[32], packetBuffer[33]);
    unsigned ntpgetTimelowWord = word(packetBuffer[34], packetBuffer[35]);
    unsigned ntpgetTimeMicrohighWord = word(packetBuffer[36], packetBuffer[37]);
    unsigned ntpgetTimeMicrolowWord = word(packetBuffer[38], packetBuffer[39]);
    unsigned long long ntpgetTime = (static_cast<unsigned long long>(ntpgetTimehighWord) << 48) | (static_cast<unsigned long long>(ntpgetTimelowWord) << 32) | (static_cast<unsigned long long>(ntpgetTimeMicrohighWord) << 16) | ntpgetTimeMicrolowWord;
    unsigned ntpsendTimehighWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned ntpsendTimelowWord = word(packetBuffer[42], packetBuffer[43]);
    unsigned ntpsendTimeMicrohighWord = word(packetBuffer[44], packetBuffer[45]);
    unsigned ntpsendTimeMicrolowWord = word(packetBuffer[46], packetBuffer[47]);
    unsigned long long ntpsendTime = (static_cast<unsigned long long>(ntpsendTimehighWord) << 48) | (static_cast<unsigned long long>(ntpsendTimelowWord) << 32) | (static_cast<unsigned long long>(ntpsendTimeMicrohighWord) << 16) | ntpsendTimeMicrolowWord;
    unsigned long long ntpserverspendTime = ntpsendTime - ntpgetTime;

    unsigned long long totalroadtime = (timegetudp - timesendudp);           //总路程时间
    totalroadtime = (totalroadtime << 32) / 1000000;                         //总路程时间毫秒
    unsigned long long roadtime = (totalroadtime - ntpserverspendTime) / 2;  //单程时间
    unsigned long long deviceGetTime = ntpsendTime + roadtime;               //设备得到UDP包时间16进制
    struct timeval tv;                                                       //创造一个结构体用于重置内部RTC
    tv.tv_sec = 0;                                                           //定义当前时刻为0时0分0秒 也就是说对时时刻为纪元原点 对时136年后时间戳溢出
    unsigned long long microtimenow = micros() + 63;                         //63位最后的计算补时即下7行代码需要消耗63微秒时间
    if (microtimenow < timegetudp) { return; }                               //如果微妙时间戳恰好溢出则对时失败
    unsigned long long calculationTime = (microtimenow - timegetudp);        //计算耗时
    calculationTime = (calculationTime << 32) / 1000000;                     //计算耗时微秒
    unsigned long long devicesetTime = deviceGetTime + calculationTime;      //计算当前时间戳
    unsigned long long microtime = (devicesetTime & 0xFFFFFFFF);             //计算当前时间秒小数
    microtime = (microtime * 1000000) >> 32;                                 //计算当前时间微秒秒数
    tv.tv_usec = microtime;                                                  //设置毫秒数
    settimeofday(&tv, NULL);                                                 //RTC对时
    unsigned long long ErastartTime = (static_cast<unsigned long long>(Era) << 32) | (devicesetTime >> 32);
    ntpTime = ErastartTime;
    // Serial.println(ErastartTime);
    // Serial.println("纪元开始时间");//正确
    // Serial.println(microtime);
    // Serial.println("纪元开始微秒");//正确

    // 保存 NTP 时间
    ntpTime -= 2208988800ULL;  // 从 1900 年转换为 Unix 时间戳
    // ntpTime += timeZone * 3600; // 考虑时区偏移
    // ntpTime *= 1000; // 转换为毫秒
    // Serial.println(ntpTime);
    // 更新全局变量
    // ntpTime = ntpTime;
    needupadte = 0;
  }
}


//时间戳获得时间
void break64Time(uint64_t timeInput, tmElement64s_t &tm) {
  // break the given time_t into time components
  // this is a more compact version of the C library localtime function
  // note that year is offset from 1970 !!!

  uint32_t year;
  uint8_t month, monthLength;
  uint64_t time;
  unsigned long days;

  time = (uint64_t)timeInput;
  tm.Second = time % 60;
  time /= 60;  // now it is minutes
  tm.Minute = time % 60;
  time /= 60;  // now it is hours
  tm.Hour = time % 24;
  time /= 24;                      // now it is days
  tm.Wday = ((time + 4) % 7) + 1;  // Sunday is day 1

  year = 0;
  days = 0;
  while ((unsigned)(days += (LEAP_YEAR(year) ? 366 : 365)) <= time) {
    year++;
  }
  tm.Year = year + 1970;  // year is offset from 1970
  // Serial.println("内部年");
  // Serial.println(year);
  days -= LEAP_YEAR(year) ? 366 : 365;
  time -= days;  // now it is days in this year, starting at 0

  days = 0;
  month = 0;
  monthLength = 0;
  for (month = 0; month < 12; month++) {
    if (month == 1) {  // february
      if (LEAP_YEAR(year)) {
        monthLength = 29;
      } else {
        monthLength = 28;
      }
    } else {
      monthLength = monthDays[month];
    }

    if (time >= monthLength) {
      time -= monthLength;
    } else {
      break;
    }
  }
  tm.Month = month + 1;  // jan is month 1
  tm.Day = time + 1;     // day of month
}
//根据时间获得时间戳
uint64_t makeTime(const tmElement64s_t &tm) {
  // assemble time elements into time_t
  // note year argument is offset from 1970 (see macros in time.h to convert to other formats)
  // previous version used full four digit year (or digits since 2000),i.e. 2009 was 2009 or 9

  int i;
  uint64_t seconds;

  // seconds from 1970 till 1 jan 00:00:00 of the given year
  seconds = tm.Year * (SECS_PER_DAY * 365);
  for (i = 0; i < tm.Year; i++) {
    if (LEAP_YEAR(i)) {
      seconds += SECS_PER_DAY;  // add extra days for leap years
    }
  }

  // add days for this year, months start from 1
  for (i = 1; i < tm.Month; i++) {
    if ((i == 2) && LEAP_YEAR(tm.Year)) {
      seconds += SECS_PER_DAY * 29;
    } else {
      seconds += SECS_PER_DAY * monthDays[i - 1];  //monthDay array starts from 0
    }
  }
  seconds += (tm.Day - 1) * SECS_PER_DAY;
  seconds += tm.Hour * SECS_PER_HOUR;
  seconds += tm.Minute * SECS_PER_MIN;
  seconds += tm.Second;
  return seconds;
}


String getalipayGET() {
  //测试函数 后期删
  String url = "https://openapi.alipay.com/gateway.do?";  // Replace with your URL
  String param = "alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&app_id=2021001194613478&biz_content={\"out_trade_no\":\"202108230101010013b\",\"total_amount\":0.01,\"subject\":\"\\u6d4b\\u8bd5\\u5546\\u54c1\",\"extend_params\":{\"sys_service_provider_id\":\"2088931278114101\"}}&charset=UTF-8&format=json&method=alipay.trade.precreate&sign_type=RSA2&timestamp=2023-06-20 10:48:51&version=1.0";

  // saveKeyValue("alipayprivatekey", newalipayprivatekey);
  String sign = alipaysign(param);
  // String sign = urlEncode(param);
  // return sign;
  url += param;
  url += "&";
  url += "sign";
  url += "=";
  url += urlEncode(sign);
  url += "&";
  url += "sign_type";
  url += "=";
  url += "RSA2";
  // Serial.println("当前支付宝URL");
  // Serial.println(url);
  return url;
  // return "https://openapi.alipay.com/gateway.do?biz_content=%7B%22out_trade_no%22%3A%22202108230101010013b%22%2C%22total_amount%22%3A0.01%2C%22subject%22%3A%22%5Cu6d4b%5Cu8bd5%5Cu5546%5Cu54c1%22%2C%22extend_params%22%3A%7B%22sys_service_provider_id%22%3A%222088931278114101%22%7D%7D&app_id=2021001194613478&version=1.0&format=json&sign_type=RSA2&method=alipay.trade.precreate&timestamp=2023-06-20+07%3A48%3A42&auth_token=&alipay_sdk=alipay-sdk-php-2020-04-15&terminal_type=&terminal_info=&prod_code=&notify_url=&charset=UTF-8&app_auth_token=&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&target_app_id=&sign=WNx39Vz3yLiEpSZ44%2B6gI47ILYpSexs33mj2AnANYsl%2FdamLY1LMYC9Qun4lHTvpBza2LiWRChh1uZc5ds2oSETrn6jY8nGJBujrY101CB%2FLaVE55DR0AD%2FAYZjrc4yL59PPNsU%2BZgf79LSveSpqUG27jhlzUY0NAYx36okWKxWpIXLgH3DHG3g4VMCW7wvKqDlssZPzdYpRDuvIfuQwtxu4H2F%2FGElAqs%2Fm2U0n06r2Jxpymo2akskkNgwDNwLirw2vtQu0Y4EdbNO4omQUKEs5fiEo4oMakrF2lE9Y7c6lXaCWJGxFJi2gIjentdDu6l6xZVbuG6Q68PTgh9BspQ%3D%3D";
}
String sendHTTPSGETalif2fpay(const char *url) {
  //const char url[] = "https://openapi.alipay.com/gateway.do?biz_content=%7B%22out_trade_no%22%3A%22202108230101010014b%22%2C%22total_amount%22%3A0.01%2C%22subject%22%3A%22%5Cu5546%5Cu54c1%22%2C%22extend_params%22%3A%7B%22sys_service_provider_id%22%3A%222088931278114101%22%7D%7D&app_id=2021001194613478&version=1.0&format=json&sign_type=RSA2&method=alipay.trade.precreate&timestamp=2023-06-26+12%3A11%3A20&auth_token=&alipay_sdk=alipay-sdk-php-2020-04-15&terminal_type=&terminal_info=&prod_code=&notify_url=&charset=UTF-8&app_auth_token=&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&target_app_id=&sign=eg%2FkZa8%2BEj5GmfZDZfpMDgnpwOOglZtN7rfeBKSerBOUuaxjEKdaxpuSuYK4RSyBmwgiWM%2BA%2BRLH4MCVSOaCC64QnMrW1bi6fRaSW%2BGXzLlyh1%2FCl5BV5TVFgFWPqPbR%2FfUj1sbs3woRc%2FJ9KM3WudojIY2uzOTl6grojy0k5BvMRcQY6Km8rAu3UwCodv9YWGxSW5AzxY%2Fi6y8gRoYSSEbKrLsLNnE6IVAShi%2Bl7QlY%2BoFXdfpieen7ou0s6lTaonkPM3xjB1ONlWKMINQi3b7FyTnlFG5jZXCzpTmWHkeGq8GFmRo9%2FMRlC0WFG%2BrTQbcEXY9lBRa%2FOGk0XE8akQ%3D%3D";
  // WiFiClientSecure *wificlient = new WiFiClientSecure;
  WiFiClientSecure wificlient;
  // wificlient -> setCACert(rootCACertificate);
  wificlient.setCACert(rootCACertificate);
  HTTPClient https;
  Serial.println(url);
  // url = "https://openapi.alipay.com/gateway.do?";
  https.begin(wificlient, String(url));
  // https.begin(*wificlient,"https://openapi.alipay.com/gateway.do?biz_content=%7B%22out_trade_no%22%3A%22202108230101010014b%22%2C%22total_amount%22%3A0.01%2C%22subject%22%3A%22%5Cu5546%5Cu54c1%22%2C%22extend_params%22%3A%7B%22sys_service_provider_id%22%3A%222088931278114101%22%7D%7D&app_id=2021001194613478&version=1.0&format=json&sign_type=RSA2&method=alipay.trade.precreate&timestamp=2023-06-26+12%3A11%3A20&auth_token=&alipay_sdk=alipay-sdk-php-2020-04-15&terminal_type=&terminal_info=&prod_code=&notify_url=&charset=UTF-8&app_auth_token=&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&target_app_id=&sign=eg%2FkZa8%2BEj5GmfZDZfpMDgnpwOOglZtN7rfeBKSerBOUuaxjEKdaxpuSuYK4RSyBmwgiWM%2BA%2BRLH4MCVSOaCC64QnMrW1bi6fRaSW%2BGXzLlyh1%2FCl5BV5TVFgFWPqPbR%2FfUj1sbs3woRc%2FJ9KM3WudojIY2uzOTl6grojy0k5BvMRcQY6Km8rAu3UwCodv9YWGxSW5AzxY%2Fi6y8gRoYSSEbKrLsLNnE6IVAShi%2Bl7QlY%2BoFXdfpieen7ou0s6lTaonkPM3xjB1ONlWKMINQi3b7FyTnlFG5jZXCzpTmWHkeGq8GFmRo9%2FMRlC0WFG%2BrTQbcEXY9lBRa%2FOGk0XE8akQ%3D%3D");
  const int httpCode = https.GET();
  if (httpCode > 0) {
    // String payload = https.getString();
    return https.getString();
    // Serial.println(https.getString());
  }
  return String("");
}
String getalipaytime(int changedsec = 0) {
  struct timeval tv;  //创造一个结构体用于重置内部RTC
  gettimeofday(&tv, NULL);
  tmElement64s_t tm;
  break64Time(ntpTime + tv.tv_sec + 28800 + changedsec, tm);
  char month[2];
  sprintf(month, "%02d", tm.Month);
  char day[2];
  sprintf(day, "%02d", tm.Day);
  char hour[2];
  sprintf(hour, "%02d", tm.Hour);
  char minute[2];
  sprintf(minute, "%02d", tm.Minute);
  char second[2];
  sprintf(second, "%02d", tm.Second);
  String result = String(tm.Year) + String("-") + String(month) + String("-") + String(day) + String(" ") + String(hour) + String(":") + String(minute) + String(":") + String(second);
  return result;
}
//直接获取支付宝当面付参数
String getalipayparam(double price, int door) {
  //  getValueByKey("lastorder")
  String doorstr = String(door);
  if (doorstr.length() < 2) {
    doorstr = "0" + doorstr;
  }
  String strMAC = WiFi.macAddress();
  strMAC.replace(":", "");
  String param = "alipay_root_cert_sn=";
  String outtradeno = String("mizhikeji.com") + doorstr + strMAC + String(alipaydealtime);
  Serial.println(outtradeno);
  Serial.println("外部订单号");

  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content={\"out_trade_no\":\"");
  param += outtradeno;
  param += String("\",\"total_amount\":");
  param += String(price, 2);
  param += String(",\"subject\":\"\\u5546\\u54c1\",\"time_expire\":\"");
  param += getalipaytime(300);  //向支付宝服务器设置交易时间限制5分钟
  param += String("\",\"extend_params\":{\"sys_service_provider_id\":\"2088931278114101\"}}&charset=UTF-8&format=json&method=alipay.trade.precreate&sign_type=RSA2&timestamp=");
  // param += String("2023-06-20 10:48:51");//getalipaytime()
  param += getalipaytime();
  param += String("&version=1.0");
  //参数全部构建完 增加签名
  String sign = alipaysign(param);
  param += "&";
  param += "sign";
  param += "=";
  param += urlEncode(sign);
  Serial.println("1412");
  return param;
}
//先预生成支付宝当面付参数后进行支付
String prealipayf2fpay(int door) {
  // 保存签名数据根据门号和价格计算出签名并保存签名
  // String price =price[door];
  String doorstr = String(door);
  if (doorstr.length() < 2) {
    doorstr = "0" + doorstr;
  }
  // String dealtime=getValueByKey("alipaydealtime");

  // int dealtimeint=0;

  struct timeval tv;  //创造一个结构体接收内部时间
  gettimeofday(&tv, NULL);
  uint64_t signtime = ntpTime + tv.tv_sec;
  Alipaysingtime[door] = signtime;  //全局变量
  alipaysigntime[door] = alipaydealtime;
  String strMAC = WiFi.macAddress();
  strMAC.replace(":", "");
  String param = "alipay_root_cert_sn=";
  String outtradeno = String("mizhikeji.com") + doorstr + strMAC + String(alipaydealtime);  // alipaydealtime为全局变量 在调用此函数时应自加1
  // Serial.println(outtradeno);Serial.println("外部订单号");

  // saveKeyValue("alipayneedasktime",String("300"));
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content={\"out_trade_no\":\"");
  param += outtradeno;
  param += String("\",\"total_amount\":");
  param += price[door];
  param += String(",\"subject\":\"\\u5546\\u54c1\",\"time_expire\":\"");
  param += getalipaytimebysmept(signtime + 300);
  ;  //向支付宝服务器设置交易时间限制5分钟
  param += String("\",\"extend_params\":{\"sys_service_provider_id\":\"2088931278114101\"}}&charset=UTF-8&format=json&method=alipay.trade.precreate&sign_type=RSA2&timestamp=");
  // param += String("2023-06-20 10:48:51");//getalipaytime()
  param += getalipaytimebysmept(signtime);
  ;  //订单生成时间
  param += String("&version=1.0");
  //参数全部构建完 增加签名

  String sign = alipaysign(param);
  param += "&";
  param += "sign";
  param += "=";
  Alipaysign[door] = sign;  //此时全局变量接收到sign
  param += urlEncode(sign);
  // Serial.println("1412");

  return param;
}

String regetprealipayf2fpay(int door) {
  // 保存签名数据根据门号和价格计算出签名并保存签名
  // String price =price[door];
  if (alipaysigntime[door] != alipaydealtime) { return String(""); }  //如果要生成的订单号和已生成的不同

  String doorstr = String(door);
  if (doorstr.length() < 2) {
    doorstr = "0" + doorstr;
  }
  // String dealtime=getValueByKey("alipaydealtime");

  // int dealtimeint=0;

  struct timeval tv;  //创造一个结构体接收内部时间
  gettimeofday(&tv, NULL);
  uint64_t signtime = ntpTime + tv.tv_sec + 28800;
  // Alipaysingtime[door]=signtime;//全局变量
  // alipaysigntime[door]=alipaydealtime;
  signtime = Alipaysingtime[door];

  String strMAC = WiFi.macAddress();
  strMAC.replace(":", "");
  String param = "alipay_root_cert_sn=";
  String outtradeno = String("mizhikeji.com") + doorstr + strMAC + String(alipaydealtime);  // alipaydealtime为全局变量 在调用此函数时应自加1
  // Serial.println(outtradeno);Serial.println("外部订单号");

  // saveKeyValue("alipayneedasktime",String("300"));
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content={\"out_trade_no\":\"");
  param += outtradeno;
  param += String("\",\"total_amount\":");
  param += price[door];
  param += String(",\"subject\":\"\\u5546\\u54c1\",\"time_expire\":\"");
  param += getalipaytimebysmept(signtime + 300);
  ;  //向支付宝服务器设置交易时间限制5分钟
  param += String("\",\"extend_params\":{\"sys_service_provider_id\":\"2088931278114101\"}}&charset=UTF-8&format=json&method=alipay.trade.precreate&sign_type=RSA2&timestamp=");
  // param += String("2023-06-20 10:48:51");//getalipaytime()
  param += getalipaytimebysmept(signtime);
  ;  //订单生成时间
  param += String("&version=1.0");
  //参数全部构建完 增加签名

  param += "&";
  param += "sign";
  param += "=";
  param += urlEncode(Alipaysign[door]);  //签名从之前的提取


  return param;
}

String getalipaytimebysmept(uint64_t timesmept) {
  struct timeval tv;  //创造一个结构体用于重置内部RTC
  gettimeofday(&tv, NULL);
  tmElement64s_t tm;
  break64Time(timesmept + 28800, tm);
  char month[2];
  sprintf(month, "%02d", tm.Month);
  char day[2];
  sprintf(day, "%02d", tm.Day);
  char hour[2];
  sprintf(hour, "%02d", tm.Hour);
  char minute[2];
  sprintf(minute, "%02d", tm.Minute);
  char second[2];
  sprintf(second, "%02d", tm.Second);
  String result = String(tm.Year) + String("-") + String(month) + String("-") + String(day) + String(" ") + String(hour) + String(":") + String(minute) + String(":") + String(second);
  return result;
}

//获取GET方式调用支付宝订单查询接口返回URL字符串
String getalipaytradequerystr(const String &tradeno) {
  String url = "/gateway.do?";
  String param = "alipay_root_cert_sn=";
  String time = getalipaytime();
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content={\"out_trade_no\":\"");
  param += tradeno;
  param += String("\"}&charset=UTF-8&format=json&method=alipay.trade.query&sign_type=RSA2&timestamp=");
  param += time;
  param += String("&version=1.0");
  String sign = alipaysign(param);
  //URLENCODE URL
  param = "alipay_root_cert_sn=";
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content=");
  param += String(urlEncode("{\"out_trade_no\":\""));
  param += urlEncode(tradeno);
  param += String(urlEncode("\"}"));
  param += String("&charset=UTF-8&format=json&method=alipay.trade.query&sign_type=RSA2&timestamp=");
  param += urlEncode(time);
  param += String("&version=1.0");


  param += "&sign=";
  param += urlEncode(sign);
  url += param;
  // char* charurlArray = new char[url.length() + 1];
  // url.toCharArray(charurlArray, url.length() + 1);
  return param;
}
//GET方式调用支付宝 订单关闭接口 返回URL字符串
String getalipaytradeclosestr(const String &tradeno) {
  String url = "/gateway.do?";
  String param = "alipay_root_cert_sn=";
  String time = getalipaytime();
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content={\"out_trade_no\":\"");
  param += tradeno;
  param += String("\"}&charset=UTF-8&format=json&method=alipay.trade.close&sign_type=RSA2&timestamp=");
  param += time;
  param += String("&version=1.0");
  String sign = alipaysign(param);
  //URLENCODE URL
  param = "alipay_root_cert_sn=";
  param += alipayrootcertsn;
  param += String("&app_cert_sn=");
  param += alipayappcertsn;
  param += String("&app_id=");
  param += alipayappid;
  param += String("&biz_content=");
  param += String(urlEncode("{\"out_trade_no\":\""));
  param += urlEncode(tradeno);
  param += String(urlEncode("\"}"));
  param += String("&charset=UTF-8&format=json&method=alipay.trade.close&sign_type=RSA2&timestamp=");
  param += urlEncode(time);
  param += String("&version=1.0");


  param += "&sign=";
  param += urlEncode(sign);
  url += param;
  // char* charurlArray = new char[url.length() + 1];
  // url.toCharArray(charurlArray, url.length() + 1);
  return param;
}



/**
 * 功能：HTTPS请求封装！ 自行封装不推荐使用本函数仅用于截取时间算法 关键通信使用的是成熟客户端算法
 * @param host：请求域名（String类型）
 * @param url：请求地址（String类型）
 * @param parameter：请求参数(String类型)(默认""")
 * @param fingerprint：服务器证书指纹 (String类型)(默认""")
 * @param Port：请求端口(int类型)(默认：443)
 * @param Receive_cache：接收缓存(int类型)(默认：1024)
 * @return 成功返回请求的内容(String类型) 失败则返回"0"
 * */
String HTTPS_request(String host, String postRequest, int Port = 443, int Receive_cache = 1024) {
  WiFiClientSecure HTTPS;  //建立WiFiClientSecure对象
  HTTPS.setCACert(rootCACertificate);
  // HTTPClient https;//不用官方库
  // https.begin(wificlient,String(url));
  // Serial.print("原始URLA");
  // Serial.println(url);
  postRequest = (String)("GET ") + postRequest + " HTTP/1.1\r\n" + "Host: " + host + "\r\n" + "User-Agent: Mozilla/5.0 (iPhone; CPU iPhone OS 13_2_3 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/13.0.3 Mobile/15E148 Safari/604.1 Edg/103.0.5060.53" + "\r\n\r\n";
  HTTPS.setInsecure();  //不进行服务器身份认证
  int cache = sizeof(postRequest) + 10;
  Serial.print("原始URLB");
  Serial.println(postRequest);
  Serial.print("发送缓存：");
  Serial.println(postRequest);
  Serial.print("发送缓存大小：");
  Serial.println(cache);
  // HTTPS.setBufferSizes(Receive_cache, cache); //接收和发送缓存大小
  HTTPS.setTimeout(15000);  //设置等待的最大毫秒数
  Serial.println("初始化参数完毕！\n开始连接服务器==>>>>>");
  if (!HTTPS.connect(host.c_str(), Port)) {
    delay(100);
    Serial.println();
    Serial.println("服务器连接失败！");
    return "0";
  } else {
    Serial.println("服务器连接成功！\r");
    Serial.println("发送请求：\n" + postRequest);
  }
  HTTPS.print(postRequest.c_str());  // 发送HTTP请求

  // 检查服务器响应信息。通过串口监视器输出服务器状态码和响应头信息
  // 从而确定ESP8266已经成功连接服务器
  Serial.println("获取响应信息========>：\r");
  Serial.println("响应头：");
  while (HTTPS.connected()) {
    String line = HTTPS.readStringUntil('\n');
    Serial.println(line);
    if (line == "\r") {
      Serial.println("响应头输出完毕！");  // Serial.println("响应头屏蔽完毕！\r");
      break;
    }
  }
  Serial.println("截取响应体==========>");
  String line;
  while (HTTPS.connected()) {
    line = HTTPS.readStringUntil('\n');  // Serial.println(line);
                                         // if (line.length() > 10)
    break;
  }
  Serial.println("响应体信息：\n" + line);
  Serial.println("====================================>");
  Serial.println("变量长度：" + String(line.length()));
  Serial.println("变量大小：" + String(sizeof(line)) + "字节");
  Serial.println("====================================>");
  HTTPS.stop();  //操作结束，断开服务器连接
  delay(500);
  return line;
}
//向https://openapi.alipay.com/gateway.do?提交请求
String Sendalipay(String param) {
  WiFiClientSecure wificlient;
  wificlient.setCACert(rootCACertificate);
  HTTPClient https;
  // Serial.println(String("https://openapi.alipay.com/gateway.do?") + param);
  // Serial.println("1572");
  https.begin(wificlient, String("https://openapi.alipay.com/gateway.do?") + param);
  // Serial.println("1574");
  const int httpCode = https.GET();
  // Serial.println("1576");
  String alipayresponse = https.getString();
  // Serial.println("1578");
  // Serial.println(alipayresponse);
  // const char input[alipayresponse.length+1] = alipayresponse.c_str;

  return alipayresponse;
}

void checkalipayorder() {
  String lastorder = getValueByKey("lastorder");
  if (lastorder.isEmpty()) { return; }
  String lastdoor = getValueByKey("lastdoor");
  if (lastdoor.isEmpty()) { return; }
  // int lastdoorint;
  // lastdoorint = lastdoor.toInt();
  lastdoor = String(lastdoor.toInt());  //去除首位的0
  String doorio = getValueByKey(String("doorio") + lastdoor);
  if (resolutionAlipaytradequeryrespon(Sendalipay(getalipaytradequerystr(lastorder)))) {
    String dooropenms = getValueByKey("dooropenms");
    pinMode(doorio.toInt(), OUTPUT);
    digitalWrite(doorio.toInt(), HIGH);
    delay(dooropenms.toInt());
    digitalWrite(doorio.toInt(), LOW);
    Serial.println("交易成功");
  } else {
    Serial.println("交易失败");
  }
}

void checkalipayorderbyalipaydealtime(uint64_t dealtime, int door) {
  String doorstr = String(door);
  if (doorstr.length() < 2) {
    doorstr = "0" + doorstr;
  }

  String strMAC = WiFi.macAddress();
  strMAC.replace(":", "");
  String param = "alipay_root_cert_sn=";
  String outtradeno = String("mizhikeji.com") + doorstr + strMAC + String(dealtime);  // alipaydealtime为全局变量 在调用此函数时应自加1


  String doorio = getValueByKey(String("doorio") + String(door));
  if (resolutionAlipaytradequeryrespon(Sendalipay(getalipaytradequerystr(outtradeno)))) {
    String dooropenms = getValueByKey("dooropenms");
    pinMode(doorio.toInt(), OUTPUT);
    digitalWrite(doorio.toInt(), HIGH);
    delay(dooropenms.toInt());
    digitalWrite(doorio.toInt(), LOW);
    Serial.println("交易成功");
  } else {
    Serial.println("交易失败");
  }
}
bool checkalipayorderbyordernumber(String outtradeno) {

  String door = outtradeno.substring(13, 15);
  String doorio = getValueByKey(String("doorio") + String(door.toInt()));
  // Serial.println(doorio);
  // Serial.println(doorio.toInt());
  if (resolutionAlipaytradequeryrespon(Sendalipay(getalipaytradequerystr(outtradeno)))) {
    String dooropenms = getValueByKey("dooropenms");
    pinMode(doorio.toInt(), OUTPUT);
    digitalWrite(doorio.toInt(), HIGH);
    delay(dooropenms.toInt());
    digitalWrite(doorio.toInt(), LOW);
    // Serial.println("交易成功");
    return true;
  } else {
    // Serial.println("交易失败");
    return false;
  }
}
bool resolutionAlipaytradequeryrespon(String alipayresponse) {

  char alipayresponseArraychar[alipayresponse.length() + 1];                             // 做一个获取MAC地址的输出缓冲区
  alipayresponse.toCharArray(alipayresponseArraychar, sizeof(alipayresponseArraychar));  // 将MAC地址输出到char类型中
  JSONVar alipayresponseArray = JSON.parse(alipayresponseArraychar);
  // Serial.println("返回值");
  // Serial.println(alipayresponse);
  // Serial.println("服务码");
  // Serial.println(httpCode);
  // Serial.println("响应码");
  // Serial.println(alipayresponseArray["alipay_trade_query_response"]["code"]);
  String alipaycode = alipayresponseArray["alipay_trade_query_response"]["code"];
  if (alipaycode.equals("10000")) {
    // Serial.println("原字符串");
    String tradestatus = alipayresponseArray["alipay_trade_query_response"]["trade_status"];
    Serial.println(tradestatus);
    if (tradestatus.equals("TRADE_CLOSED")) {
      // Serial.println("交易超时关闭");
      return false;
    }

    if (tradestatus.equals("TRADE_SUCCESS")) {
      // Serial.println("交易成功");
      return true;
    }
    if (tradestatus.equals("TRADE_FINISHED")) {
      // Serial.println("交易结束");
      return true;
    }
  }

  return false;
}


void setup() {
  Serial.begin(115200);
  // Serial.println("hello");
  digitalWrite(0, LOW);
  digitalWrite(1, LOW);
  digitalWrite(2, LOW);
  digitalWrite(3, LOW);
  digitalWrite(4, LOW);
  digitalWrite(5, LOW);
  digitalWrite(6, LOW);
  digitalWrite(7, LOW);
  digitalWrite(8, LOW);
  digitalWrite(9, LOW);
  digitalWrite(10, LOW);
  digitalWrite(11, LOW);
  digitalWrite(12, LOW);
  digitalWrite(13, LOW);
  digitalWrite(18, LOW);
  digitalWrite(19, LOW);
  // digitalWrite(20, LOW);
  // digitalWrite(21, LOW);
  pinMode(0, OUTPUT);
  pinMode(1, OUTPUT);
  pinMode(2, OUTPUT);
  pinMode(3, OUTPUT);
  pinMode(4, OUTPUT);
  pinMode(5, OUTPUT);
  pinMode(6, OUTPUT);
  pinMode(7, OUTPUT);
  pinMode(8, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT);
  pinMode(11, OUTPUT);
  pinMode(12, OUTPUT);
  pinMode(13, OUTPUT);
  pinMode(18, OUTPUT);
  pinMode(19, OUTPUT);
  // pinMode(20, OUTPUT);
  // pinMode(21, OUTPUT);
  // Serial.println("hello1");


  xTaskCreate(taskcleanwebpassworderrortime, "Task1", 1000, NULL, 1, &task1Handle);  //开启密码错误计时器如果计时器大于0则禁止其输入密码
  if (!LittleFS.begin(true)) {                                                       //初始化LittleFS 如果初始化失败就尝试格式化 如果还失败就串口输出
    Serial.println("LittleFS initialization failed!");
    digitalWrite(ERROE_PIN, HIGH);
    // while (1)
    ;  //如果文件系统加载失败则执行死锁
  }



  // digitalWrite(LED_PIN, HIGH);
  // Serial.println(WiFi.macAddress());
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }



  //如果存在配置文件则载入
  String buf;


  //尝试获取启动次数如果没有启动次数则加1
  buf = getValueByKey("opentime");
  if (buf.equals("")) {
    //初始配置密码
    saveKeyValue("websetpassword", String("56778890"));  //

    saveKeyValue("doorio1", String("1"));
    saveKeyValue("doorio2", String("2"));
    saveKeyValue("doorio3", String("3"));
    saveKeyValue("doorio4", String("4"));
    saveKeyValue("doorio5", String("5"));
    saveKeyValue("doorio6", String("6"));
    saveKeyValue("doorio7", String("7"));
    saveKeyValue("doorio8", String("8"));
    saveKeyValue("doorio9", String("9"));
    saveKeyValue("doorio10", String("10"));
    saveKeyValue("doorio11", String("11"));
    saveKeyValue("doorio12", String("12"));
    saveKeyValue("doorio13", String("13"));
    saveKeyValue("doorio14", String("18"));
    saveKeyValue("doorio15", String("19"));
    saveKeyValue("doorio16", String("20"));
    saveKeyValue("doorio17", String("21"));
    saveKeyValue("doorio18", String("0"));
    doorio[1] = 1;
    doorio[2] = 2;
    doorio[3] = 3;
    doorio[4] = 4;
    doorio[5] = 5;
    doorio[6] = 6;
    doorio[7] = 7;
    doorio[8] = 8;
    doorio[9] = 9;
    doorio[10] = 10;
    doorio[11] = 11;
    doorio[12] = 12;
    doorio[13] = 13;
    doorio[14] = 18;
    doorio[15] = 19;
    doorio[16] = 20;
    doorio[17] = 21;
    doorio[18] = 0;





    saveKeyValue("price1", String("0"));
    saveKeyValue("price2", String("0"));
    saveKeyValue("price3", String("0"));
    saveKeyValue("price4", String("0"));
    saveKeyValue("price5", String("0"));
    saveKeyValue("price6", String("0"));
    saveKeyValue("price7", String("0"));
    saveKeyValue("price8", String("0"));
    saveKeyValue("price9", String("0"));
    saveKeyValue("price10", String("0"));
    saveKeyValue("price11", String("0"));
    saveKeyValue("price12", String("0"));
    saveKeyValue("price13", String("0"));
    saveKeyValue("price14", String("0"));
    saveKeyValue("price15", String("0"));
    saveKeyValue("price16", String("0"));
    saveKeyValue("price17", String("0"));
    saveKeyValue("price18", String("0"));
    //阿里默认根证书
    saveKeyValue("alipayhttpspublickeykey", String(rootCACertificate));
    //默认无剩余轮询时间
    saveKeyValue("alipayneedasktime", String("0"));
    //订单号起点
    saveKeyValue("alipaydealtime", String("0"));
    // saveKeyValue("alipayprivatekey", newalipayprivatekey);
    // saveKeyValue("alipayrootcertsn", alipayrootcertsn);
    // saveKeyValue("appcertsn", appcertsn);
    // saveKeyValue("appid", appid);


    saveKeyValue("opentime", String("1"));  //避免下次执行继续调用本代码
  } else {
    // unsigned long long i = buf.toInt();
    // i++;
    // saveKeyValue("opentime", String(i));
    websetpassword = getValueByKey("websetpassword");
    i = 18;
    while (i--) {
      doorio[i] = getValueByKey("doorio" + String(i)).toInt();
      price[i] = getValueByKey("price" + String(i));
    }
  };
  // writeFile(LittleFS,"/setpassword.txt",setpassword);
  //如果存储的密码不为空则重置密码
  buf = getValueByKey("websetpassword");
  if (buf.equals("")) {
  } else {
    websetpassword = buf;
  };

  buf = getValueByKey("alipayrootcertsn");
  if (buf.equals("")) {
  } else {
    alipayrootcertsn = buf;
  };

  buf = getValueByKey("appcertsn");
  if (buf.equals("")) {
  } else {
    alipayappcertsn = buf;
  };
  buf = getValueByKey("appid");
  if (buf.equals("")) {
  } else {
    alipayappid = buf;
  };
  alipayprivatekey = getValueByKey("alipayprivatekey");

  buf = getValueByKey("alipaydealtime");
  if (buf.equals("")) {
  } else {
    char charArray[buf.length() + 1];
    buf.toCharArray(charArray, sizeof(charArray));
    alipaydealtime = strtoull(charArray, NULL, 10);
  };
  buf = getValueByKey("alipayneedclosetrytime");
  if (buf.equals("")) {
  } else {
    char charArray[buf.length() + 1];
    buf.toCharArray(charArray, sizeof(charArray));
    alipayneedclosetrytime = strtoull(charArray, NULL, 10);
  };

  buf = getValueByKey("alipayneedquerytrytime");
  if (buf.equals("")) {
  } else {
    char charArray[buf.length() + 1];
    buf.toCharArray(charArray, sizeof(charArray));
    alipayneedquerytrytime = strtoull(charArray, NULL, 10);
  };

  buf = getValueByKey("alipayneedqueryorder");
  if (buf.equals("")) {
  } else {
    alipayneedqueryorder = buf;
  };


  buf = getValueByKey("alipayneedcloseorder");
  if (buf.equals("")) {
  } else {
    alipayneedcloseorder = buf;
  };


  //websetpassword
  // saveKeyValue("key1", "value333\new==a12");
  // String value1 = alipaysign("a=1");
  // Serial.println("Value1: " + value1);
  // Serial.println(websetpassword);
  //setalipayprivatekey
  //开启UDP服务器监听收到的对时包
  udp.listen(localPort);
  udp.onPacket([](AsyncUDPPacket packet) {
    // 处理接收到的 UDP 数据包
    handleNtpResponse(packet);
  });

  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_GET, handleControl);
  server.on("/time", HTTP_GET, handleecho);
  server.on("/set", HTTP_GET, handleset);
  server.on("/setwebpassword", HTTP_GET, handlesetwebpassword);
  server.on("/setwebpassword2", HTTP_GET, handlesetwebpassword2);
  server.on("/setalipayprivatekey", HTTP_GET, handlesetalipayprivatekey);  //handlesetalipayprivatekey2
  //存在HTTP拦截 窃取支付宝私钥的可能，所以请在安全的网络上传密钥
  server.on("/setalipayprivatekey2", HTTP_GET, handlesetalipayprivatekey2);
  server.on("/setalipaypublickey", HTTP_GET, handlesetalipaypublickey);
  server.on("/setalipaypublickeykey2", HTTP_GET, handlesetalipaypublickey2);
  server.on("/setprice", HTTP_GET, handlesetprice);
  server.on("/setprice2", HTTP_GET, handlesetprice2);
  server.on("/setprice", HTTP_GET, handlesetprice);
  server.on("/setprice2", HTTP_GET, handlesetprice2);
  server.on("/setdoorpin", HTTP_GET, handlesetdoorpin);
  server.on("/setdoorpin2", HTTP_GET, handlesetdoorpin2);
  server.on("/buy", HTTP_GET, handlebuy);  //购买确认页 返回请求二维码的HTTP请求页给前端
  server.on("/b", HTTP_GET, handleb);      //直接购买页，返回一个JS页面读取并发包给buy页

  server.on("/qrcode.min.js", HTTP_ANY, handlesetqrcodeminjs);
  server.begin();

  Serial.println("Server started");
}

void loop() {
  // WiFiClientSecure *wificlient = new WiFiClientSecure;
  // String alipayhttpscert = getValueByKey("alipayhttpspublickeykey");
  // const char *alipayhttpscertchar = alipayhttpscert.c_str();

  // wificlient -> setCACert(alipayhttpscertchar);
  // HTTPClient https;
  // https.begin(*wificlient, "https://wlw.liyaodong.cn/");
  // int httpCode = https.GET();
  // String payload = https.getString();
  // Serial.println(payload);
  // Serial.println("当前支付宝参数");
  // Serial.println(getalipayGET());
  // Serial.println(getValueByKey("alipayprivatekey"));
  // Serial.println(getValueByKey("alipayrootcertsn"));
  // Serial.println(getValueByKey("appcertsn"));
  // Serial.println(getValueByKey("appid"));

  struct timeval tv;  //创造一个结构体用于重置内部RTC
  gettimeofday(&tv, NULL);
  // Serial.println(tv.tv_sec);
  if (tv.tv_sec > 864000) { needupadte = 1; }  //如果大于10天就申请授时 10天RTC与标准时间差最大约20S影响不大

  if (needupadte) {
    //HTTPS授时
    HTTPS_GETTIME("openapi.alipay.com", "/gateway.do?method=alipay.trade.query", 443, 1024);  //乱请求根据响应头的内容获得时间
  }
  if (needupadte) {
    getNtpTimeAsync();
    delay(3000);
    Serial.println("等待授时");
  }
  /*
  else {
    Serial.println(getalipaytime());

    // const char* url="https://openapi.alipay.com/gateway.do?alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&app_id=2021001194613478&biz_content={%22out_trade_no%22:%22202108230101010012b%22,%22total_amount%22:1.00,%22subject%22:%22\u5546\u54c1%22,%22extend_params%22:{%22sys_service_provider_id%22:%222088931278114101%22}}&charset=UTF-8&format=json&method=alipay.trade.precreate&sign_type=RSA2&timestamp=2023-06-20%2010:48:51&version=1.0&sign=gG%2BD%2BOOBC5fH%2F6zSN3pOm7J09s2VSbzBCJsuadW3f%2FoX56%2Fki0W1N%2FjzCujQta7HYZGg3cM%2BU8KBO8R0lspyrnVGrEXHlz%2FLEVIaMyfcWQJJTFokvZ6%2BuOe%2B0YROMdCY%2FCdQdfLNOCkaYhOHDrUxpvhV6%2FZZaKorLS4NhkU%2F9VQiKmLi2rpFlDGwLUYYaDNH1kkqUuw2IytUl8mXcHG8LEL2kMad874ngtUx7KjKUuewe70Rc7mDNcvyTcwN0UKNi0FNCUMbo5LSFLLvtp%2BxkOCVB%2BcUlvt4CSnvy8Jn5%2B84IjauPIdI5pT5qcxj7WbtisYrYBn5ySZ7nJgxqnfeAg%3D%3D";
    // char *url=alipaytradequery("202108230101010012b");
    // Serial.println(alipaytradequery("202108230101010012b"));
    // HTTPS_request("openapi.alipay.com",alipaytradequery("202108230101010012b"),443,1024);
    // HTTPS_request("openapi.alipay.com","/gateway.do?alipay_root_cert_sn=687b59193f3f462dd5336e5abf83c5d8_02941eef3187dddf3d3b83462e1dfcf6&app_cert_sn=0fbfea6f1c396ed1ee67302f2d035201&app_id=2021001194613478&biz_content=%7B%22out_trade_no%22%3A%22202108230101010012b%22%7D&charset=UTF-8&format=json&method=alipay.trade.query&sign_type=RSA2&timestamp=2023-06-28%2005:53:27&version=1.0&sign=hqvaUWh7XsCntWc%2BIbaTK2T%2FXNfHd%2BrB6yedCfe7AlcLY65BKWp7aBhG7mtdE32tAHq9lWFouAAVJqqBhZyUhTAHr4awFMg62%2BfyYUbL6b6JRmzxr0b1ofm8X4LMRlFZu3GRJYXvEUSM5DXkJZbrnpPJnPOhoTh%2BqpdcZNYGV2aebL1r19vC1sdr7jfi9D4L6i%2F3SG6885zqKDJQZtU7kkszHjhfA4FombxrHJSJm1V5pxzU2GsEegXD0iFHhINgiksIWbfsdH6MSW4jfld6r2M3SisBjp9%2BGyNQ9fnV6g9Vxenj3%2FKoFdDyKrXFM1wGWmJitUI19dZsUMG4zUbqpA%3D%3D",443,1024);


    // const char *alipayhttpscertchar = getValueByKey("alipayhttpspublickeykey").c_str();
    // String buf = sendHTTPSGETalif2fpay(alipaytradequery("202108230101010012b"));


    // const char* url = alipaytradequery("202108230101010012b");
    // String buf = sendHTTPSGETalif2fpay(url);
    WiFiClientSecure wificlient;
    wificlient.setCACert(rootCACertificate);
    HTTPClient https;
    // Serial.println(url);
    // https.begin(alipaytradequery("202108230101010012b"),rootCACertificate);
    https.begin(wificlient, String("https://openapi.alipay.com") + alipaytradequery("202108230101010012b"));
    const int httpCode = https.GET();
    Serial.println("返回值");
    Serial.println(https.getString());
    Serial.println("服务码");
    Serial.println(httpCode);

    // delete[] url;


    // String buf = sendHTTPSGETalif2fpay(alipaytradequery("202108230101010012b"));
    // Serial.println(buf);
    // alipaytradequery()
    //
    // delete[] url;
  }
  */
  // if (!needupadte) {
  // Serial.println(alipaytradeclose("202108230101010016b"));
  // Serial.println(Sendalipay(getalipaytradequerystr("202108230101010015b")));
  // if (resolutionAlipaytradequeryrespon(Sendalipay(getalipaytradequerystr("202108230101010015b")))) {
  //   Serial.println("交易成功");
  // } else {
  //   Serial.println("交易失败");
  // }
  // }

  //   if (resolutionAlipaytradequeryrespon(Sendalipay(getalipaytradequerystr("mizhikeji.com130.026055F9745C781")))) {
  //   Serial.println("交易成功");
  // } else {
  //   Serial.println("交易失败");
  // }
  // Serial.println("测试预生产URL");
  // Serial.println(prealipayf2fpay(1));
  // Serial.println("测试重置URL");
  // Serial.println(regetprealipayf2fpay(1));
  // checkalipayorder();
  if (!needupadte) {
    ///循环预计算签名
    prealipayf2fpay(alipyneedsigndoor);
    alipyneedsigndoor++;
    if (alipyneedsigndoor >= 19) { alipyneedsigndoor = 1; }
    //轮询是否存在需要关闭的订单
    struct timeval tv;  //创造一个结构体用于重置内部RTC
    gettimeofday(&tv, NULL);
    tmElement64s_t tm;
    uint64_t timesmeptnow = ntpTime + tv.tv_sec;  //获得当前时间戳
    //如果未到关闭后查单的轮询上限就不停的查
    if (alipayneedclosetrytime >= timesmeptnow) {
      // Serial.println("尝试关闭订单");
      Sendalipay(getalipaytradeclosestr(alipayneedcloseorder));
      // Serial.println(Sendalipay(getalipaytradeclosestr(alipayneedcloseorder)));
      // Serial.println("尝试查询订单");
      //如果查询成功则不再轮询
      if (checkalipayorderbyordernumber(alipayneedcloseorder)) {
        alipayneedclosetrytime = 0;
        saveKeyValue("alipayneedclosetrytime", String("0"));
      };
    }
    //查询当前订单
    if (alipayneedquerytrytime >= timesmeptnow) {
      Serial.println("尝试轮询订单");
      if (checkalipayorderbyordernumber(alipayneedqueryorder)) {
        Serial.println("订单交易成功");
        alipayneedquerytrytime = 0;
        saveKeyValue("alipayneedquerytrytime", String("0"));                   //如果查询到订单号就停止轮询
        lastsuccessalipayorder = alipayneedqueryorder;                         //将最后成交订单号保存至全局变量
        saveKeyValue("lastsuccessalipayorder", String(alipayneedqueryorder));  //将最后成交订单号保存至硬盘
      };
    }
  }
  // if(checkalipayorderbyordernumber(String("mizhikeji.com026055F9745C783"))){
    
  //   Serial.println("轮询为真");
  // };

}
