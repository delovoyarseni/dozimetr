#ifndef CONFIG_H
#define CONFIG_H

#define PIN_TFT_DC        9
#define PIN_TFT_RST       10
#define PIN_TFT_BLK       8      

#define PIN_BUZZER        4      
#define PIN_LED           3      
#define PIN_GEIGER        2      
#define PIN_VBAT          A0     

#define PIN_BTN_LEFT      5
#define PIN_BTN_MID       6
#define PIN_BTN_RIGHT     7

#define COLOR565(r, g, b)   ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define COL_BG            COLOR565(0, 0, 0)
#define COL_UI_BG         COLOR565(28, 28, 28)
#define COL_PANEL         COLOR565(64, 64, 64)
#define COL_PANEL_EDGE    COLOR565(120, 120, 120)
#define COL_TEXT          COLOR565(235, 235, 235)
#define COL_TEXT_DIM      COLOR565(150, 150, 150)
#define COL_GOOD          COLOR565(0, 255, 0)
#define COL_WARN          COLOR565(255, 255, 0)
#define COL_BAD           COLOR565(255, 0, 0)
#define COL_BLUE          COLOR565(0, 90, 255)
#define COL_GRAPH_FILL    COLOR565(0, 128, 128)
#define COL_GRAPH_LINE    COLOR565(0, 230, 230)
#define COL_GRID          COLOR565(70, 70, 70)


#define GEIGER_EDGE_MODE      0         
#define GEIGER_PULLUP         1         
#define GEIGER_DEAD_US        150       
#define GEIGER_CPM_PER_USVH   180.0f    
#define GEIGER_WINDOW_S       60        
#define GEIGER_MIN_S          20        
#define USV_TO_UR             100.0f    

#define DOSE_GOOD_MAX         0.30f     
#define DOSE_WARN_MAX         0.60f     
#define ZONE_HYSTERESIS       0.90f     

#define GRAPH_POINTS          47        
#define GRAPH_STEP_USV        0.20f     
#define GRAPH_STEP_UR         20.0f     
#define GRAPH_STEP_CPM        50.0f     
#define GRAPH_MIN_TOP_STEPS   2         

#define BAT_R_TOP             33000.0f  
#define BAT_R_BOTTOM          9500.0f   
#define ADC_REF_MV            1067.0f   
#define BAT_PERIOD_MS         2000UL
#define BAT_CAPACITY_MAH      2000      
#define BAT_CURRENT_MA        150       

#define CLICK_MS              3         
#define VOLUME_TEST_MS        120       
#define LED_TEST_MS           400       
#define RING_PATTERN          200, 30, 200, 30, 200, 30, 200, 900, 200, 30, 200, 30, 200, 30, 200, 900, 200, 30, 200, 30, 200, 30, 200, 900
#define RING_STEPS            24
#define ALERT_ON_MS           10000UL
#define ALERT_PAUSE_MS        10000UL
#define LED_BLINK_MS          200
#define LED_PULSE_MS          25

#define BTN_DEBOUNCE_MS       25
#define BTN_LONG_MS           700
#define SNAKE_LEN             14        
#define SNAKE_STEP_MS         35
#define BTN_SNAKE_LEN         16        
#define BTN_SNAKE_STEP_MS     30
#define BTN_SNAKE_SPEED       2         
#define FADE_STEP_MS          28        
#define SAVED_MS              900       
#define WAVE_STEP_MS          110       
#define BOOT_FADE_STEPS       14
#define BOOT_STEP_MS          55
#define BOOT_HOLD_MS          700
#define PWM_LEVELS            16        
#define PWM_TICK_OCR          249

#define FW_VERSION            "f27102026"
#define GITHUB_NAME           "delovoyarseni/dozimetr"

#define CFG_VERSION           4         
#define DEF_BRIGHTNESS        8         
#define DEF_SLEEP             0         
#define DEF_ANIM              1         
#define DEF_UNIT              0         
#define DEF_GRAPH_STEP        1         
#define DEF_SOUND             1         
#define DEF_VOLUME            9         
#define DEF_CLICKS            1         
#define DEF_ALERT             1         
#define DEF_LED_MODE          2         
#define DEF_LED_BRIGHT        4         
#define DEF_LOW_BAT           1         

#endif