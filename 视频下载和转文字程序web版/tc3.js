// tc3.js —— 腾讯云 TC3-HMAC-SHA256 签名（Node 原生 crypto 实现）
// 参考官方签名文档：https://cloud.tencent.com/document/api/213/30654

'use strict';

const crypto = require('crypto');

function tc3Sign(opts) {
  const { secretId, secretKey, service, action, payloadStr, timestamp } = opts;
  // date 必须用 UTC
  const date = new Date(timestamp * 1000).toISOString().slice(0, 10);
  const host = service + '.tencentcloudapi.com';

  const hashedPayload = crypto.createHash('sha256').update(payloadStr, 'utf8').digest('hex');
  const canonicalHeaders = 'content-type:application/json; charset=utf-8\n' + 'host:' + host + '\n';
  const signedHeaders = 'content-type;host';
  const canonicalRequest =
    'POST' + '\n' +
    '/' + '\n' +
    '' + '\n' +
    canonicalHeaders + '\n' +
    signedHeaders + '\n' +
    hashedPayload;

  const credentialScope = date + '/' + service + '/tc3_request';
  const hashedCR = crypto.createHash('sha256').update(canonicalRequest, 'utf8').digest('hex');
  const stringToSign =
    'TC3-HMAC-SHA256' + '\n' +
    timestamp + '\n' +
    credentialScope + '\n' +
    hashedCR;

  const kDate = crypto.createHmac('sha256', 'TC3' + secretKey).update(date).digest();
  const kService = crypto.createHmac('sha256', kDate).update(service).digest();
  const kSigning = crypto.createHmac('sha256', kService).update('tc3_request').digest();
  const signature = crypto.createHmac('sha256', kSigning).update(stringToSign).digest('hex');

  const authorization =
    'TC3-HMAC-SHA256 ' +
    'Credential=' + secretId + '/' + credentialScope + ', ' +
    'SignedHeaders=' + signedHeaders + ', ' +
    'Signature=' + signature;

  return { authorization, host };
}

module.exports = { tc3Sign };
