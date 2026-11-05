/* Classic worker wrapper for Tesseract: installs the Pagenoodle network guard, then loads the
 * locally-served tesseract.js worker. All model/core files come from this origin. */
(function () {
  var base = self.location.href;
  function same(u) {
    try {
      var url = new URL(String(u), base);
      return url.protocol === 'blob:' || url.protocol === 'data:' || url.origin === new URL(base).origin;
    } catch (e) {
      return false;
    }
  }
  var f = self.fetch && self.fetch.bind(self);
  if (f) self.fetch = function (input, init) {
    var u = typeof input === 'string' || input instanceof URL ? input : input.url;
    if (!same(u)) return Promise.reject(new TypeError('Pagenoodle privacy guard blocked ' + u));
    return f(input, init);
  };
  var open = XMLHttpRequest.prototype.open;
  XMLHttpRequest.prototype.open = function (m, u) {
    if (!same(u)) throw new TypeError('Pagenoodle privacy guard blocked ' + u);
    return open.apply(this, arguments);
  };
  var imp = self.importScripts.bind(self);
  self.importScripts = function () {
    for (var i = 0; i < arguments.length; i++) if (!same(arguments[i])) throw new TypeError('Pagenoodle privacy guard blocked ' + arguments[i]);
    return imp.apply(self, arguments);
  };
  ['WebSocket', 'EventSource', 'WebTransport', 'RTCPeerConnection', 'fetchLater'].forEach(function (n) {
    if (n in self) self[n] = function () { throw new TypeError('Pagenoodle privacy guard blocked ' + n); };
  });
})();
importScripts('../vendor/tesseract/worker.min.js');
