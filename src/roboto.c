#include "pebble_os.h"
#include "pebble_app.h"
#include "pebble_fonts.h"

#include "http.h"
#include "util.h"
#include "weather_layer.h"
#include "time_layer.h"
#include "link_monitor.h"
#include "config.h"
#include "suncalc.h"
#include "my_math.h"

#define MY_UUID { 0x91, 0x41, 0xB6, 0x28, 0xBC, 0x89, 0x49, 0x8E, 0xB1, 0x47, 0x04, 0x9F, 0x49, 0xC0, 0x99, 0xAD }

PBL_APP_INFO(MY_UUID,
             "MDS ForecastWeather", "Michael Smith",
             1, 7, /* App version */
             RESOURCE_ID_IMAGE_MENU_ICON,
             APP_INFO_WATCH_FACE);

#define TIME_FRAME      (GRect(0, 2, 144, 168-6))
#define DATE_FRAME      (GRect(0, 55, 144, 168-62))  //(GRect(0, 58, 144, 168-62))

// POST variables
#define WEATHER_KEY_LATITUDE 1
#define WEATHER_KEY_LONGITUDE 2
#define WEATHER_KEY_UNIT_SYSTEM 3
	
// Received variables
#define WEATHER_KEY_ICON 1
#define WEATHER_KEY_TEMPERATURE 2
#define WEATHER_KEY_FCSTHIGH 3
#define WEATHER_KEY_FCSTLOW 4
#define WEATHER_KEY_FCST   5
	
#define WEATHER_HTTP_COOKIE 1949327671
#define TIME_HTTP_COOKIE 1131038282

Window window;          /* main window */
TextLayer date_layer;   /* layer for the date */
TimeLayer time_layer;   /* layer for the time */
TextLayer text_sunrise_layer;
TextLayer text_sunset_layer;
TextLayer text_fcst_layer;
//TextLayer text_cond_layer;

char fcstlow_text[5];
char fcsthigh_text[5];
char fcstcond_text[60];
char fcst_text[40];

GFont font_date;        /* font for date (normal) */
GFont font_hour;        /* font for hour (bold) */
GFont font_minute;      /* font for minute (thin) */
GFont font_sun;		/* font for sunrise(condensed) */
GFont font_fcst;	/* font for forecast(condensed) */

static int initial_minute;

//Weather Stuff
static int our_latitude, our_longitude, our_timezone = 99;;
static bool located = false;
static bool calculated_sunset_sunrise = false;

WeatherLayer weather_layer;

void request_weather();

void fcst_layer_set_forecast(int16_t hi, int16_t lo, char* cond) {
	memcpy(fcstlow_text, itoa(lo), 4);
	memcpy(fcsthigh_text, itoa(hi), 4);
	memcpy(fcstcond_text, cond, strlen(cond));
	fcstcond_text[strlen(fcstcond_text)] = '\0';
	
	strcpy(fcst_text, fcstlow_text);
	strcat(fcst_text, "° / ");
	strcat(fcst_text, fcsthigh_text);
	strcat(fcst_text, "°  ");
	strcat(fcst_text, fcstcond_text);
	//strcat(fcst_text, fcstcond_text);
	text_layer_set_text(&text_fcst_layer, fcst_text);
	//text_layer_set_text(&text_cond_layer, fcstcond_text);
	
}

void failed(int32_t cookie, int http_status, void* context) {
	if(cookie == 0 || cookie == WEATHER_HTTP_COOKIE) {
		weather_layer_set_icon(&weather_layer, WEATHER_ICON_NO_WEATHER);
		text_layer_set_text(&weather_layer.temp_layer, "---°");
	}
	
	link_monitor_handle_failure(http_status);
	
	//Re-request the location and subsequently weather on next minute tick
	located = false;
}

void success(int32_t cookie, int http_status, DictionaryIterator* received, void* context) {
	if(cookie != WEATHER_HTTP_COOKIE) return;
	Tuple* icon_tuple = dict_find(received, WEATHER_KEY_ICON);
	if(icon_tuple) {
		int icon = icon_tuple->value->int8;
		if(icon >= 0 && icon < 16) {
			weather_layer_set_icon(&weather_layer, icon);
		} else {
			weather_layer_set_icon(&weather_layer, WEATHER_ICON_NO_WEATHER);
		}
	}
	Tuple* temperature_tuple = dict_find(received, WEATHER_KEY_TEMPERATURE);
	if(temperature_tuple) {
		weather_layer_set_temperature(&weather_layer, temperature_tuple->value->int16);
	}
	
	Tuple* fcstlow_tuple = dict_find(received, WEATHER_KEY_FCSTLOW);
	Tuple* fcsthigh_tuple = dict_find(received, WEATHER_KEY_FCSTHIGH);
	Tuple* fcstcond_tuple = dict_find(received, WEATHER_KEY_FCST);
	if(fcstlow_tuple) {
		fcst_layer_set_forecast( fcsthigh_tuple->value->int16, fcstlow_tuple->value->int16, 
		fcstcond_tuple->value->cstring);
	}
	
	
	/*static char fcstlow_text[]  = "";
	static char fcsthigh_text[]  = "";
	//static char fcstcond_text[]  = "";
	static char fcst_text[]  = "";
	
	memcpy(fcstlow_text, itoa(fcstlow_tuple->value->int16), fcstlow_tuple->length);
	
	
	memcpy(fcsthigh_text, itoa(fcsthigh_tuple->value->int16), fcsthigh_tuple->length);
	
	//Tuple* fcstcond_tuple = dict_find(received, WEATHER_KEY_FCST);
	//memcpy(fcstcond_text, fcstcond_tuple->value->cstring, strlen(fcstcond_tuple->value->cstring));
	//fcstcond_text[strlen(fcstcond_tuple->value->cstring)] = '\0';
	
	strcat(fcst_text, fcstlow_text);
	strcat(fcst_text, "° / ");
	strcat(fcst_text, fcsthigh_text);
	strcat(fcst_text, "°  ");
	//strcat(fcst_text, fcstcond_text);
	text_layer_set_text(&text_fcst_layer, fcst_text); */	
	link_monitor_handle_success();
}

void location(float latitude, float longitude, float altitude, float accuracy, void* context) {
	// Fix the floats
	our_latitude = latitude * 10000;
	our_longitude = longitude * 10000;
	located = true;
	request_weather();
}

void reconnect(void* context) {
	located = false;
	request_weather();
}

void request_weather();

/* Called by the OS once per minute. Update the time and date.
*/

void adjustTimezone(float* time) 
{
  *time += our_timezone;
  if (*time > 24) *time -= 24;
  if (*time < 0) *time += 24;
}

void updateSunsetSunrise()
{
	// Calculating Sunrise/sunset with courtesy of Michael Ehrmann
	// https://github.com/mehrmann/pebble-sunclock
	static char sunrise_text[] = "00:00";
	static char sunset_text[]  = "00:00";

	PblTm pblTime;
	get_time(&pblTime);

	char *time_format;

	if (clock_is_24h_style()) 
	{
	  time_format = "%R";
	} 
	else 
	{
	  time_format = "%I:%M";
	}

	float sunriseTime = calcSunRise(pblTime.tm_year, pblTime.tm_mon+1, pblTime.tm_mday, our_latitude / 10000, our_longitude / 10000, 91.0f);
	float sunsetTime = calcSunSet(pblTime.tm_year, pblTime.tm_mon+1, pblTime.tm_mday, our_latitude / 10000, our_longitude / 10000, 91.0f);
	adjustTimezone(&sunriseTime);
	adjustTimezone(&sunsetTime);

	if (!pblTime.tm_isdst) 
	{
	  sunriseTime+=1;
	  sunsetTime+=1;
	} 

	pblTime.tm_min = (int)(60*(sunriseTime-((int)(sunriseTime))));
	pblTime.tm_hour = (int)sunriseTime;
	string_format_time(sunrise_text, sizeof(sunrise_text), time_format, &pblTime);
	//text_layer_set_text(&text_sunrise_layer, sunrise_text);

	pblTime.tm_min = (int)(60*(sunsetTime-((int)(sunsetTime))));
	pblTime.tm_hour = (int)sunsetTime;
	string_format_time(sunset_text, sizeof(sunset_text), time_format, &pblTime);
	//text_layer_set_text(&text_sunset_layer, sunset_text);
}

void receivedtime(int32_t utc_offset_seconds, bool is_dst, uint32_t unixtime, const char* tz_name, void* context)
{	
	our_timezone = (utc_offset_seconds / 3600);
	if (is_dst)
	{
		our_timezone--;
	}

/*	if (located && our_timezone != 99 && !calculated_sunset_sunrise)
    {
        updateSunsetSunrise();
	    calculated_sunset_sunrise = true;
    } */
}

void handle_minute_tick(AppContextRef ctx, PebbleTickEvent *t)
{
    /* Need to be static because pointers to them are stored in the text
    * layers.
    */
    static char date_text[] = "XXX, XXX 00";
    static char hour_text[] = "00";
    static char minute_text[] = ":00";

    (void)ctx;  /* prevent "unused parameter" warning */

    if (t->units_changed & DAY_UNIT)
    {
        string_format_time(date_text,
                           sizeof(date_text),
                           "%a, %b %d",
                           t->tick_time);
        text_layer_set_text(&date_layer, date_text);
    }

    if (clock_is_24h_style())
    {
        string_format_time(hour_text, sizeof(hour_text), "%H", t->tick_time);
    }
    else
    {
        string_format_time(hour_text, sizeof(hour_text), "%I", t->tick_time);
        if (hour_text[0] == '0')
        {
            /* This is a hack to get rid of the leading zero.
            */
            memmove(&hour_text[0], &hour_text[1], sizeof(hour_text) - 1);
        }
    }

    string_format_time(minute_text, sizeof(minute_text), ":%M", t->tick_time);
    time_layer_set_text(&time_layer, hour_text, minute_text);
	
	if(!located || (t->tick_time->tm_min % 5) == initial_minute)
	{
		//Every 5 minutes, request updated weather
		http_location_request();
	}
	else
	{
		//Every minute, ping the phone
		link_monitor_ping();
	}
}


/* Initialize the application.
*/
void handle_init(AppContextRef ctx)
{
    PblTm tm;
    PebbleTickEvent t;
    ResHandle res_d;
    ResHandle res_h;
    ResHandle res_m;
    ResHandle res_s;

    window_init(&window, "Roboto");
    window_stack_push(&window, true /* Animated */);
    window_set_background_color(&window, GColorBlack);

    resource_init_current_app(&APP_RESOURCES);

    res_d = resource_get_handle(RESOURCE_ID_FONT_ROBOTO_CONDENSED_21);
    res_h = resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49);
    res_m = resource_get_handle(RESOURCE_ID_FONT_ROBOTO_BOLD_SUBSET_49);
   // res_s = resource_get_handle(RESOURCE_ID_GOTHIC_14);

    font_date = fonts_load_custom_font(res_d);
    font_hour = fonts_load_custom_font(res_h);
    font_minute = fonts_load_custom_font(res_m);
   // font_sun    = fonts_load_custom_font(res_s);
    font_fcst = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    
    //Forecast Text
    
	text_layer_init(&text_fcst_layer, window.layer.frame);
	text_layer_set_text_color(&text_fcst_layer, GColorWhite);
	text_layer_set_background_color(&text_fcst_layer, GColorClear);
	layer_set_frame(&text_fcst_layer.layer, GRect(7, 133, 100, 25));
	text_layer_set_font(&text_fcst_layer, font_fcst);
	layer_add_child(&window.layer, &text_fcst_layer.layer);
	
//	text_layer_init(&text_cond_layer, window.layer.frame);
//	text_layer_set_text_color(&text_cond_layer, GColorWhite);
//	text_layer_set_background_color(&text_cond_layer, GColorClear);
//	layer_set_frame(&text_cond_layer.layer, GRect(7, 144, 100, 25));
//	text_layer_set_font(&text_cond_layer, font_fcst);
//	layer_add_child(&window.layer, &text_cond_layer.layer);
    
    	// Sunrise Text
//	text_layer_init(&text_sunrise_layer, window.layer.frame);
//	text_layer_set_text_color(&text_sunrise_layer, GColorWhite);
//	text_layer_set_background_color(&text_sunrise_layer, GColorClear);
//	layer_set_frame(&text_sunrise_layer.layer, GRect(7, 143, 100, 30));
//	text_layer_set_font(&text_sunrise_layer, font_sun);
//	layer_add_child(&window.layer, &text_sunrise_layer.layer);

	// Sunset Text
//	text_layer_init(&text_sunset_layer, window.layer.frame);
//	text_layer_set_text_color(&text_sunset_layer, GColorWhite);
//	text_layer_set_background_color(&text_sunset_layer, GColorClear);
//	layer_set_frame(&text_sunset_layer.layer, GRect(94, 143, 100, 30));
//	text_layer_set_font(&text_sunset_layer, font_sun);
//	layer_add_child(&window.layer, &text_sunset_layer.layer); 

    time_layer_init(&time_layer, window.layer.frame);
    time_layer_set_text_color(&time_layer, GColorWhite);
    time_layer_set_background_color(&time_layer, GColorClear);
    time_layer_set_fonts(&time_layer, font_hour, font_minute);
    layer_set_frame(&time_layer.layer, TIME_FRAME);
    layer_add_child(&window.layer, &time_layer.layer);

    text_layer_init(&date_layer, window.layer.frame);
    text_layer_set_text_color(&date_layer, GColorWhite);
    text_layer_set_background_color(&date_layer, GColorClear);
    text_layer_set_font(&date_layer, font_date);
    text_layer_set_text_alignment(&date_layer, GTextAlignmentCenter);
    layer_set_frame(&date_layer.layer, DATE_FRAME);
    layer_add_child(&window.layer, &date_layer.layer);

	// Add weather layer
	weather_layer_init(&weather_layer, GPoint(0, 72)); //0,80  //0, 100
	layer_add_child(&window.layer, &weather_layer.layer);
	
	http_register_callbacks((HTTPCallbacks){.failure=failed,.success=success,.reconnect=reconnect,.location=location,.time=receivedtime}, (void*)ctx);
	
	// Refresh time
	get_time(&tm);
        t.tick_time = &tm;
        t.units_changed = SECOND_UNIT | MINUTE_UNIT | HOUR_UNIT | DAY_UNIT;
	
	//initial_minute = (tm.tm_min % 30);
	
	handle_minute_tick(ctx, &t);
}

/* Shut down the application
*/
void handle_deinit(AppContextRef ctx)
{
    fonts_unload_custom_font(font_date);
    fonts_unload_custom_font(font_hour);
    fonts_unload_custom_font(font_minute);
	
	weather_layer_deinit(&weather_layer);
}


/********************* Main Program *******************/

void pbl_main(void *params)
{
    PebbleAppHandlers handlers =
    {
        .init_handler = &handle_init,
        .deinit_handler = &handle_deinit,
        .tick_info =
        {
            .tick_handler = &handle_minute_tick,
            .tick_units = MINUTE_UNIT
        },
		.messaging_info = {
			.buffer_sizes = {
				.inbound =  124,
				.outbound = 124,
			}
		}
    };

    app_event_loop(params, &handlers);
}

void request_weather() {
	if(!located) {
		http_location_request();
		return;
	}
	// Build the HTTP request
	DictionaryIterator *body;
	HTTPResult result = http_out_get("http://12.189.158.76/cgi-bin/weather.py", WEATHER_HTTP_COOKIE, &body);
	if(result != HTTP_OK) {
		weather_layer_set_icon(&weather_layer, WEATHER_ICON_NO_WEATHER);
		return;
	}
	dict_write_int32(body, WEATHER_KEY_LATITUDE, our_latitude);
	dict_write_int32(body, WEATHER_KEY_LONGITUDE, our_longitude);
	dict_write_cstring(body, WEATHER_KEY_UNIT_SYSTEM, UNIT_SYSTEM);
	// Send it.
	if(http_out_send() != HTTP_OK) {
		weather_layer_set_icon(&weather_layer, WEATHER_ICON_NO_WEATHER);
		return;
	}
}
