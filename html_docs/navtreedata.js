/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "ARA SDK 1.9.14", "index.html", [
    [ "Introduction", "intro.html", [
      [ "Welcome", "intro.html#sec_Welcome", null ],
      [ "About ARA", "intro.html#autotoc_md0", null ]
    ] ],
    [ "ARA Design Overview", "ara_design_overview.html", [
      [ "ARA API technical design overview", "ara_design_overview.html#sec_ARATechnicalDesign", [
        [ "Relationship between ARA and established plug-in APIs", "ara_design_overview.html#sec_ARACompanionAPIs", null ],
        [ "Language and Platform Support", "ara_design_overview.html#sec_LanguageAndPlatformSupport", null ],
        [ "API Versioning", "ara_design_overview.html#autotoc_md1", null ],
        [ "Objects and Object References", "ara_design_overview.html#sec_ARA_Object_References", null ]
      ] ],
      [ "The ARA document controller", "ara_design_overview.html#sec_ARA_Document_Controller", null ],
      [ "ARA model graph overview", "ara_design_overview.html#sec_ModelGraphOverview", null ],
      [ "Exchanging content information", "ara_design_overview.html#autotoc_md2", null ],
      [ "Updating the ARA Model Graph", "ara_design_overview.html#autotoc_md3", null ],
      [ "ARA model persistency", "ara_design_overview.html#autotoc_md4", null ],
      [ "Host Signal Flow and Threading", "ara_design_overview.html#autotoc_md5", null ],
      [ "Inserting ARA into the Signal Flow", "ara_design_overview.html#autotoc_md6", null ],
      [ "Plug-In Instance Roles", "ara_design_overview.html#sec_PlugInInstanceRoles", null ],
      [ "Audio Access and Threading", "ara_design_overview.html#sec_AudioAccessAndThreading", null ]
    ] ],
    [ "Implementing ARA", "implementing_a_r_a.html", [
      [ "Utilizing the examples and existing products", "implementing_a_r_a.html#sec_UtilizingExamples", [
        [ "Mini Host", "implementing_a_r_a.html#autotoc_md7", null ],
        [ "Test Host and Test Plug-In", "implementing_a_r_a.html#autotoc_md8", null ],
        [ "JUCE_ARA", "implementing_a_r_a.html#autotoc_md9", null ],
        [ "Existing Products", "implementing_a_r_a.html#autotoc_md10", null ]
      ] ],
      [ "Integrating the ARA SDK into your products", "implementing_a_r_a.html#autotoc_md11", [
        [ "ARAInterface, Debug", "implementing_a_r_a.html#autotoc_md12", null ],
        [ "C++ Dispatcher, Utilities", "implementing_a_r_a.html#autotoc_md13", null ],
        [ "ARAPlug", "implementing_a_r_a.html#autotoc_md14", null ],
        [ "JUCE_ARA", "implementing_a_r_a.html#autotoc_md15", null ]
      ] ],
      [ "Mapping the Internal Model to ARA", "implementing_a_r_a.html#autotoc_md16", [
        [ "Dealing with overlapping Playback Regions", "implementing_a_r_a.html#autotoc_md17", null ],
        [ "What should Audio Modifications represent in a host?", "implementing_a_r_a.html#autotoc_md18", null ],
        [ "What do Region Sequences represent?", "implementing_a_r_a.html#autotoc_md19", null ]
      ] ],
      [ "Configuring the rendering", "implementing_a_r_a.html#autotoc_md20", [
        [ "Setting up an ARA Playback Renderer", "implementing_a_r_a.html#autotoc_md21", null ],
        [ "Sample rate conversion upon playback", "implementing_a_r_a.html#autotoc_md22", null ],
        [ "Playback region head and tail times", "implementing_a_r_a.html#autotoc_md23", null ],
        [ "Dealing with denormals", "implementing_a_r_a.html#autotoc_md24", null ],
        [ "Caching especially CPU-intense DSP", "implementing_a_r_a.html#autotoc_md25", null ]
      ] ],
      [ "Analyzing audio material", "implementing_a_r_a.html#autotoc_md26", [
        [ "What can be analyzed", "implementing_a_r_a.html#autotoc_md27", null ],
        [ "Manual adjustments", "implementing_a_r_a.html#autotoc_md28", null ],
        [ "Triggering explicit analysis", "implementing_a_r_a.html#autotoc_md29", null ],
        [ "Algorithm selection", "implementing_a_r_a.html#autotoc_md30", null ]
      ] ],
      [ "Utilizing content exchange", "implementing_a_r_a.html#autotoc_md31", [
        [ "Musical Timing Information, including Content Grade examples", "implementing_a_r_a.html#autotoc_md32", null ],
        [ "Notes, including examples of transformations affecting content data", "implementing_a_r_a.html#autotoc_md33", null ],
        [ "Chords, Key Signatures and other content types", "implementing_a_r_a.html#autotoc_md34", null ]
      ] ],
      [ "Manipulating the timing", "implementing_a_r_a.html#sec_manipulatingTiming", null ],
      [ "Content Based Fades", "implementing_a_r_a.html#autotoc_md35", null ],
      [ "Managing ARA archives", "implementing_a_r_a.html#autotoc_md36", null ],
      [ "Partial Persistency", "implementing_a_r_a.html#sec_PartialPersistency", [
        [ "Copying ARA data between documents", "implementing_a_r_a.html#autotoc_md37", null ],
        [ "Audio File Chunks", "implementing_a_r_a.html#autotoc_md38", null ]
      ] ],
      [ "User interface considerations", "implementing_a_r_a.html#autotoc_md39", null ],
      [ "Choosing Companion APIs", "implementing_a_r_a.html#autotoc_md40", null ],
      [ "VST3 specific considerations", "implementing_a_r_a.html#sec_vst3Considerations", [
        [ "setActive vs. setProcessing", "implementing_a_r_a.html#autotoc_md41", null ],
        [ "View Embedding", "implementing_a_r_a.html#sec_ViewEmbedding", null ],
        [ "View Scaling", "implementing_a_r_a.html#autotoc_md42", null ]
      ] ],
      [ "Audio Unit specific considerations", "implementing_a_r_a.html#autotoc_md43", [
        [ "Buffer allocation", "implementing_a_r_a.html#autotoc_md44", null ]
      ] ],
      [ "Future ARA development", "implementing_a_r_a.html#autotoc_md45", null ]
    ] ],
    [ "ARA Use Cases and Testing", "test_cases.html", [
      [ "Synopsis", "test_cases.html#autotoc_md46", null ],
      [ "Render Timing", "test_cases.html#autotoc_md47", null ],
      [ "Musical Timing", "test_cases.html#autotoc_md48", null ],
      [ "Time Stretching", "test_cases.html#autotoc_md49", null ],
      [ "Signal Flow and Routing", "test_cases.html#autotoc_md50", null ],
      [ "Maintaining the ARA Model", "test_cases.html#autotoc_md51", null ],
      [ "Audio-MIDI Conversion", "test_cases.html#autotoc_md52", null ],
      [ "Key Signatures and Chords", "test_cases.html#autotoc_md53", null ],
      [ "Persistence", "test_cases.html#autotoc_md54", null ],
      [ "Versioning", "test_cases.html#autotoc_md55", null ],
      [ "UI-Related Topics", "test_cases.html#autotoc_md56", null ],
      [ "General", "test_cases.html#autotoc_md57", null ]
    ] ],
    [ "ARA API Reference", "modules.html", "modules" ]
  ] ]
];

var NAVTREEINDEX =
[
"ara_design_overview.html",
"group___a_r_a___library___a_r_a_plug___plug_in_instance_roles.html#a5d37657f73531dc97869a5f84df4a4d3",
"group___a_r_a___library___host___dispatch___plug-_in___interfaces.html#class_a_r_a_1_1_plug_in_1_1_plug_in_extension_instance",
"group___model___content___readers__and___content___events.html#gga24de378fe088e091ac0bdd0467f5bcb8a385ef0e6519c5b89ceeec18d1a293bd3",
"implementing_a_r_a.html#autotoc_md10"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';