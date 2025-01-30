#!/usr/bin/env node

/*
 * A test server
 */
'use strict';

var expressStaticGzip = require("express-static-gzip");
var express = require('express');
var http = require('http');
var ws = require('ws');
var multiparty = require('multiparty');

var app = new express();

var server = http.createServer(app);

var wss = new ws.Server({ server });

var wssConn;

app.use(function(req, res, next) {
    console.log(req.originalUrl);
    next();
});

app.use(expressStaticGzip("src"));

var pages = {
		"type":"sv.init.menu",
		"value": [
			{"1": { "url" : "clock.html", "title" : "Clock" }},
			{"2": { "url" : "leds.html", "title" : "LEDs" }},
			{"3": { "url" : "extra.html", "title" : "Extra" }},
			{"4": { "url" : "info.html", "title" : "Info" }}
		]
	}


var sendValues = function(conn, screen) {
}

var sendPages = function(conn) {
	var json = JSON.stringify(pages);
	conn.send(json);
	console.log(json);
}

var sendLEDValues = function(conn) {
	var json = '{"type":"sv.init.leds","value":';
	json += JSON.stringify(state[2]);
	json += '}';
	console.log(json);
	conn.send(json);
}

var sendClockValues = function(conn) {
	var json = '{"type":"sv.init.clock","value":';
	json += JSON.stringify(state[1]);
	json += '}';
	console.log(json);
	conn.send(json);
}

var sendExtrasValues = function(conn) {
	var json = '{"type":"sv.init.extras","value":';
	json += JSON.stringify(state[3]);
	json += '}';
	console.log(json);
	conn.send(json);
}

var sendInfoValues = function(conn) {
	var json = '{"type":"sv.init.info","value":';
	json += JSON.stringify(state[4]);
	json += '}';
	console.log(json);
	conn.send(json);
}

var state = {
	"1": {
		"date_format":1,
		"time_or_date":0,
		"hour_format":true,
		"display_on":0,
		"display_off":24,
		"off_state_off":1,
		"effect": 1,
		"ripple_direction": false,
		"ripple_speed": true,
		"time_zone":"EST5EDT,M3.2.0,M11.1.0"
	},
	"2": {
		"backlights":false,
		"backlight_red":7,
		"backlight_green":7,
		"backlight_blue":7,
		"underlights":false,
		"underlight_red":7,
		"underlight_green":7,
		"underlight_blue":7,
		"baselights":false,
		"baselight_red":7,
		"baselight_green":7,
		"baselight_blue":7
	},
	"3": {
		"command":"a command"
	},
	"4": {
		'esp_boot_version' : "1234",
		'esp_free_heap' : "5678",
		'esp_sketch_size' : "90123",
		'esp_sketch_space' : "4567",
		'esp_flash_size' : "8901",
		'esp_chip_id' : "chip id",
		'wifi_ip_address' : "192.168.1.1",
		'wifi_mac_address' : "0E:12:34:56:78",
		'wifi_ssid' : "STC-Wonderful",
		'up_time' : "2567 days 12:00:01"
	}
}

var broadcastUpdate = function(conn, field, value) {
	var json = '{"type":"sv.update","value":{' + '"' + field + '":' + JSON.stringify(value) + '}}';
	console.log(json);
	try {
		conn.send(json);
	} catch (e) {
		console.log(e);
	}
}

var updateValue = function(conn, screen, pair) {
	console.log(screen);
	console.log(pair);
	var index = pair.indexOf(':');

	var key = pair.substring(0, index);
	var value = pair.substring(index+1);
	try {
		value = JSON.parse(value);
	} catch (e) {

	}

	state[screen][key] = value;

	broadcastUpdate(conn, key, state[screen][key]);
}

var consoleData = [];
var valCount = 0;

var updateConsole = function(conn) {
	if (consoleData.length === 20) {
		consoleData.shift(); // Remove the first (oldest) element
	}
	consoleData.push("This is a very long value that will hopefull overflow the right hand side value <high> " + ++valCount);

	broadcastUpdate(conn, "console_data", consoleData);
}

wss.on('connection', function(conn) {
	wssConn = conn;

    console.log('connected');
	var hueTimer = setInterval(updateConsole, 2000, conn);

    //connection is up, let's add a simple simple event
	conn.on('message', function(data, isBinary) {

        //log the received message and send it back to the client
        console.log('received: %s', data);
        var message = isBinary ? data : data.toString();
    	var code = parseInt(message.substring(0, message.indexOf(':')));

    	switch (code) {
    	case 0:
    		sendPages(conn);
    		break;
    	case 1:
    		sendClockValues(conn);
    		break;
    	case 2:
    		sendLEDValues(conn);
    		break;
		case 3:
			sendExtrasValues(conn);
			break;
		case 4:
			sendInfoValues(conn);
			break;
		case 9:
    		message = message.substring(message.indexOf(':')+1);
    		var screen = message.substring(0, message.indexOf(':'));
    		var pair = message.substring(message.indexOf(':')+1);
    		updateValue(conn, screen, pair);
    		break;
    	}
    });

	conn.on('close', function() {
		clearInterval(hueTimer);
	});
});

//start our server
server.listen(process.env.PORT || 8080, function() {
    console.log('Server started on port' + server.address().port + ':)');
});

