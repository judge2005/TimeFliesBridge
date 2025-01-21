#include <WSMenuHandler.h>

String WSMenuHandler::clockMenu = "{\"1\": { \"url\" : \"clock.html\", \"title\" : \"Clock\" }}";
String WSMenuHandler::ledsMenu = "{\"2\": { \"url\" : \"leds.html\", \"title\" : \"LEDs\" }}";
String WSMenuHandler::extraMenu = "{\"3\": { \"url\" : \"extra.html\", \"title\" : \"Extra\" }}";
String WSMenuHandler::infoMenu = "{\"4\": { \"url\" : \"info.html\", \"title\" : \"Info\" }}";

void WSMenuHandler::handle(AsyncWebSocketClient *client, char *data) {
	String json("{\"type\":\"sv.init.menu\", \"value\":[");
	char *sep = "";
	for (int i=0; items[i] != 0; i++) {
		json.concat(sep);json.concat(*items[i]);sep=",";
	}
	json.concat("]}");
	client->text(json);
}

void WSMenuHandler::setItems(String **items) {
	this->items = items;
}

