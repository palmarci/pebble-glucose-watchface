// Settings schema for Clay (https://github.com/pebble/clay), rendered as the watchapp's
// "Settings" page in the Pebble mobile app. Values are sent to the watchapp as AppMessage on
// save (see index.js); main.c relays the alert-popup fields on to the firmware's alert filter.
module.exports = [
  {
    "type": "heading",
    "defaultValue": "Pump alert popups"
  },
  {
    "type": "text",
    "defaultValue": "Which pump alarms/alerts should vibrate and pop up on the watch. The pump " +
      "itself still alarms regardless of what's picked here -- this only controls the watch popup."
  },
  {
    "type": "toggle",
    "messageKey": "AlertsLow",
    "label": "Low BG alerts",
    "description": "Predicted low, low, and severe low glucose alerts.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "AlertsOther",
    "label": "Other pump alerts",
    "description": "Everything else: reservoir, battery, sensor, SmartGuard, calibration, etc.",
    "defaultValue": false
  },
  {
    "type": "heading",
    "defaultValue": "Graph"
  },
  {
    "type": "slider",
    "messageKey": "GraphHours",
    "label": "Graph length (hours)",
    "description": "How much BG history the graph shows.",
    "min": 1,
    "max": 6,
    "step": 1,
    "defaultValue": 2
  },
  {
    "type": "slider",
    "messageKey": "StaleMinutes",
    "label": "Stale after (minutes)",
    "description": "How long without a new reading before the BG/IOB numbers blank out.",
    "min": 10,
    "max": 60,
    "step": 5,
    "defaultValue": 15
  },
  {
    "type": "heading",
    "defaultValue": "Hypo alert"
  },
  {
    "type": "text",
    "defaultValue": "The on-watch model that decides TREAT vs WATCH for a falling low."
  },
  {
    "type": "slider",
    "messageKey": "HypoTreatThreshold",
    "label": "TREAT threshold (%)",
    "description": "Lower catches more real lows but flags more false alarms; higher is the " +
      "opposite. 32% is the model's own validated cutoff -- change it only if you have a reason to.",
    "min": 10,
    "max": 90,
    "step": 1,
    "defaultValue": 32
  },
  {
    "type": "toggle",
    "messageKey": "HypoVibrate",
    "label": "Vibrate on TREAT",
    "description": "Turn off to keep the TREAT/WATCH band visual-only.",
    "defaultValue": true
  },
  {
    "type": "heading",
    "defaultValue": "Features"
  },
  {
    "type": "text",
    "defaultValue": "Turn a feature off to hide it and stop asking the phone for its data."
  },
  {
    "type": "toggle",
    "messageKey": "ShowMeals",
    "label": "Meals",
    "description": "The fork icon and carb count on the graph.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "ShowHypo",
    "label": "Hypo TREAT/WATCH",
    "description": "The falling-low decision band below the graph.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "ShowPrediction",
    "label": "Forecast line",
    "description": "The dotted projection past the latest reading.",
    "defaultValue": true
  },
  {
    "type": "toggle",
    "messageKey": "ShowTrend",
    "label": "Trend arrow",
    "description": "The rising/falling arrow row.",
    "defaultValue": true
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];
