/**
 * Copyright © Jordan Lee, Dave Perrett, Malcolm Jarvis and Bruno Bierbaumer
 *
 * This file is licensed under the GPLv2.
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 */

var RPC = {
	_DaemonVersion          : 'version',
	_DownSpeedLimit         : 'speed-limit-down',
	_DownSpeedLimited       : 'speed-limit-down-enabled',
	_QueueMoveTop           : 'queue-move-top',
	_QueueMoveBottom        : 'queue-move-bottom',
	_QueueMoveUp            : 'queue-move-up',
	_QueueMoveDown          : 'queue-move-down',
	_Root                   : '../rpc',
	_TurtleDownSpeedLimit   : 'alt-speed-down',
	_TurtleState            : 'alt-speed-enabled',
	_TurtleUpSpeedLimit     : 'alt-speed-up',
	_UpSpeedLimit           : 'speed-limit-up',
	_UpSpeedLimited         : 'speed-limit-up-enabled'
};

function TransmissionRemote(controller)
{
	this.initialize(controller);
	return this;
}

TransmissionRemote.prototype =
{
	/*
	 * Constructor
	 */
	initialize: function(controller) {
		this._controller = controller;
		this._error = '';
		this._token = '';
	},

	/*
	 * Display an error if an ajax request fails, and stop sending requests
	 * or on a 409, globally set the X-Transmission-Session-Id and resend
	 */
	ajaxError: function(request, error_string, exception, ajaxObject) {
		var token,
		   remote = this;

		// set the Transmission-Session-Id on a 409
		if (request.status === 409 && (token = request.getResponseHeader('X-Transmission-Session-Id'))){
			remote._token = token;
			$.ajax(ajaxObject);
			return;
		}

		remote._error = request.responseText
		              ? request.responseText.trim().replace(/(<([^>]+)>)/ig,"")
		              : "";
		if (!remote._error.length)
			remote._error = 'Server not responding';

		dialog.confirm('Connection Failed',
			'Could not connect to the server. You may need to reload the page to reconnect.',
			'Details',
			function() {
				alert(remote._error);
			},
			'Dismiss');
		remote._controller.togglePeriodicSessionRefresh(false);
	},

	appendSessionId: function(XHR) {
		if (this._token) {
			XHR.setRequestHeader('X-Transmission-Session-Id', this._token);
		}
	},

	sendRequest: function(data, callback, context, async) {
		var remote = this;
		if (typeof async != 'boolean')
			async = true;

		var ajaxSettings = {
			url: RPC._Root,
			type: 'POST',
			contentType: 'json',
			dataType: 'json',
			cache: false,
			data: JSON.stringify(data),
			beforeSend: function(XHR){ remote.appendSessionId(XHR); },
			error: function(request, error_string, exception){ remote.ajaxError(request, error_string, exception, ajaxSettings); },
			success: callback,
			context: context,
			async: async
		};

		$.ajax(ajaxSettings);
	},

	loadDaemonPrefs: function(callback, context, async) {
		var o = { method: 'session-get' };
		this.sendRequest(o, callback, context, async);
	},

	checkPort: function(callback, context, async) {
		var o = { method: 'port-test' };
		this.sendRequest(o, callback, context, async);
	},

	renameTorrent: function(torrentIds, oldpath, newname, callback, context) {
		var o = {
			method: 'torrent-rename-path',
			arguments: {
				'ids': torrentIds,
				'path': oldpath,
				'name': newname
			}
		};
		this.sendRequest(o, callback, context);
	},

	loadDaemonStats: function(callback, context, async) {
		var o = { method: 'session-stats' };
		this.sendRequest(o, callback, context, async);
	},

	updateTorrents: function(torrentIds, fields, callback, context) {
		var o = {
			method: 'torrent-get',
			arguments: {
				'fields': fields
			}
		};
		if (torrentIds)
			o['arguments'].ids = torrentIds;
		this.sendRequest(o, function(response) {
			var args = response['arguments'];
			callback.call(context,args.torrents,args.removed);
		});
	},

	getFreeSpace: function(dir, callback, context) {
		var remote = this;
		var o = {
			method: 'free-space',
			arguments: { path: dir }
		};
		this.sendRequest(o, function(response) {
			var args = response['arguments'];
			callback.call (context, args.path, args['size-bytes']);
		});
	},

	changeFileCommand: function(torrentId, fileIndices, command) {
		var remote = this,
		    args = { ids: [torrentId] };
		args[command] = fileIndices;
		this.sendRequest({
			arguments: args,
			method: 'torrent-set'
		}, function() {
			remote._controller.refreshTorrents([torrentId]);
		});
	},

	sendTorrentSetRequests: function(method, torrent_ids, args, callback, context) {
		if (!args) args = { };
		args['ids'] = torrent_ids;
		var o = {
			method: method,
			arguments: args
		};
		this.sendRequest(o, callback, context);
	},

	sendTorrentActionRequests: function(method, torrent_ids, callback, context) {
		this.sendTorrentSetRequests(method, torrent_ids, null, callback, context);
	},

	startTorrents: function(torrent_ids, noqueue, callback, context) {
		var name = noqueue ? 'torrent-start-now' : 'torrent-start';
		this.sendTorrentActionRequests(name, torrent_ids, callback, context);
	},
	stopTorrents: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests('torrent-stop', torrent_ids, callback, context);
	},

	moveTorrents: function(torrent_ids, new_location, callback, context) {
		var remote = this;
		this.sendTorrentSetRequests( 'torrent-set-location', torrent_ids,
			{"move": true, "location": new_location}, callback, context);
	},

	setGroupTorrents: function(torrent_ids, new_group, callback, context) {
		var args = {
		    	ids: torrent_ids,
		    	downloadGroup: new_group
		    };
		this.sendRequest({
			arguments: args,
			method: 'torrent-set'
		}, callback, context);
	},

	removeTorrents: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests('torrent-remove', torrent_ids, callback, context);
	},
	removeTorrentsAndData: function(torrents) {
		var remote = this;
		var o = {
			method: 'torrent-remove',
			arguments: {
				'delete-local-data': true,
				ids: [ ]
			}
		};

		if (torrents) {
			for (var i=0, len=torrents.length; i<len; ++i) {
				o.arguments.ids.push(torrents[i].getId());
			}
		}
		this.sendRequest(o, function() {
			remote._controller.refreshTorrents();
		});
	},
	verifyTorrents: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests('torrent-verify', torrent_ids, callback, context);
	},
	reannounceTorrents: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests('torrent-reannounce', torrent_ids, callback, context);
	},
	addTorrentByUrl: function(url, options) {
		var remote = this;
		if (url.match(/^[0-9a-f]{40}$/i)) {
			url = 'magnet:?xt=urn:btih:'+url;
		}
        else {
            var hash = ' ';
            var hashValid = false;
            if (/^t magnet:\?/i.test(url)) {
                const magnetData = /xt=urn:btih:([a-z0-9]+)/i;
                const magnetHash = magnetData.exec(url);
                if (magnetHash) {
                   hash = magnetHash[1];
                   hash = hash.toUpperCase();
                }
            }
            else if (/^t /i.test(url)) {
                const hashData = /t ([a-z0-9]+)/i;
                const hashHash = hashData.exec(url);
                if (hashHash) {
                   hash = hashHash[1];
                   hash = hash.toUpperCase();
                }
            }
            if (hash.length === 32 && /\b[A-Z2-7]{32}\b/.test(hash)) {

                 /*
                 Nibbler - Multi-Base Encoder
                 version 2010-04-07 mini
                 Copyright (c) 2010 Thomas Peri
                 http://www.tumuski.com/
                 MIT License
                 */
                var Nibbler=function(A){var B,C,D,E,F,G,H,I,J,K,L,M,N;B=function(){var x,y,z;C=A.pad||'';D=A.dataBits;E=A.codeBits;F=A.keyString;G=A.arrayData;y=Math.max(D,E);z=0;H=[];for(x=0;x<y;x+=1){H.push(z);z+=z+1;}J=z;I=D/K(D,E);};K=function(u,v){var w;while(v!==0){w=v;v=u%v;u=w;}return u;};L=function(f,g,h,j){var k,l,m,o,p,q,r,s;s=function(e){if(!j){r.push(F.charAt(e));}else if(G){r.push(e);}else{r.push(String.fromCharCode(e));}};p=0;q=0;r=[];l=f.length;for(k=0;k<l;k+=1){q+=g;if(j){m=f.charAt(k);o=F.indexOf(m);if(m===C){break;}else if(o<0){throw'the character "'+m+'" is not a member of '+F;}}else{if(G){o=f[k];}else{o=f.charCodeAt(k);}if((o|J)!==J){throw o+" is outside the range 0-"+J;}}p=(p<<g)|o;while(q>=h){q-=h;s(p>>q);p&=H[q];}}if(!j&&q>0){s(p<<(h-q));l=r.length%I;for(k=0;k<l;k+=1){r.push(C);}}return(G&&j)?r:r.join('');};M=function(d){return L(d,D,E,false);};N=function(c){return L(c,E,D,true);};this.encode=M;this.decode=N;B();};

                const base32 = new Nibbler({
                    dataBits: 8,
                    codeBits: 5,
                    keyString: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567',
                    pad: '='
                });
                const base16 = new Nibbler({
                    dataBits: 8,
                    codeBits: 4,
                    keyString: '0123456789ABCDEF'
                });

                hash = base16.encode(base32.decode(hash));
                hashValid = true;
            }
            else if (hash.length === 40 && (/\b[A-F0-9]{40}\b/.test(hash))) {
                hashValid = true;
            }

            if (hashValid)
                url = 'http://itorrents.org/torrent/' + hash + '.torrent';
        }
		var o = {
			method: 'torrent-add',
			arguments: {
				paused: (options.paused),
				filename: url
			}
		};
		this.sendRequest(o, function(response) {
			remote._controller.refreshTorrents();
			if ((response.result != 'success') && (response.result != 'duplicate torrent') && hashValid) {
				url = 'http://torrasave.site/torrent/' + hash + '.torrent';
				o = {
					method: 'torrent-add',
					arguments: {
						paused: (options.paused),
						filename: url
					}
				};
				remote.sendRequest(o, function(response) {
					remote._controller.refreshTorrents();
					if ((response.result != 'success') && (response.result != 'duplicate torrent') && hashValid) {
					url = 'http://thetorrent.org/' + hash + '.torrent';
					alert('\r\nAllow pop-up to' + '\r\nDownload torrent by URL' + '\r\nadding torrent by URL\r\n"' + url + '"');
					var win = window.open(url, '_blank');
					win.focus();
					url = 'http://btcache.me/torrent/' + hash;
					alert('\r\nAllow pop-up to' + '\r\nDownload torrent by URL' + '\r\nadding torrent by URL\r\n"' + url + '"');
					var win2 = window.open(url, '_blank');
					win2.focus();
					} else {
						alert(response.result + '\r\nadding torrent by URL\r\n"' + url + '"');
					}
				});
			} else {
				alert(response.result + '\r\nadding torrent by URL\r\n"' + url + '"');
			}
		});
	},
	savePrefs: function(args, callback) {
		var remote = this;
		var o = {
			method: 'session-set',
			arguments: args
		};
		if(typeof(callback) === 'undefined') {
			callback = function() {
				remote._controller.loadDaemonPrefs();
			};
		}
		this.sendRequest(o, callback);
	},
	updateBlocklist: function() {
		var remote = this;
		var o = {
			method: 'blocklist-update'
		};
		this.sendRequest(o, function() {
			remote._controller.loadDaemonPrefs();
		});
	},

	// Added queue calls
	moveTorrentsToTop: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests(RPC._QueueMoveTop, torrent_ids, callback, context);
	},
	moveTorrentsToBottom: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests(RPC._QueueMoveBottom, torrent_ids, callback, context);
	},
	moveTorrentsUp: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests(RPC._QueueMoveUp, torrent_ids, callback, context);
	},
	moveTorrentsDown: function(torrent_ids, callback, context) {
		this.sendTorrentActionRequests(RPC._QueueMoveDown, torrent_ids, callback, context);
	}
};
