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
  [ "ARA SDK 2.1.0", "index.html", [
    [ "Introduction", "intro.html", [
      [ "Welcome", "intro.html#sec_Welcome", null ],
      [ "About ARA", "intro.html#autotoc_md0", null ]
    ] ],
    [ "ARA Design Overview", "ara_design_overview.html", [
      [ "Technical Design Overview", "ara_design_overview.html#sec_ARATechnicalDesign", [
        [ "Relationship Between ARA And Established Plug-In APIs", "ara_design_overview.html#sec_ARACompanionAPIs", null ],
        [ "Language And Platform Support", "ara_design_overview.html#sec_LanguageAndPlatformSupport", null ],
        [ "API Versioning", "ara_design_overview.html#autotoc_md1", null ],
        [ "Objects And Object References", "ara_design_overview.html#sec_ARA_Object_References", null ]
      ] ],
      [ "The ARA Document Controller", "ara_design_overview.html#sec_ARA_Document_Controller", null ],
      [ "ARA Model Graph Overview", "ara_design_overview.html#sec_ModelGraphOverview", null ],
      [ "Exchanging Content Information", "ara_design_overview.html#autotoc_md2", null ],
      [ "Updating The ARA Model Graph", "ara_design_overview.html#autotoc_md3", null ],
      [ "ARA Model Persistency", "ara_design_overview.html#autotoc_md4", null ],
      [ "Host Signal Flow And Threading", "ara_design_overview.html#autotoc_md5", null ],
      [ "Inserting ARA Into The Signal Flow", "ara_design_overview.html#autotoc_md6", null ],
      [ "Plug-In Instance Roles", "ara_design_overview.html#sec_PlugInInstanceRoles", null ],
      [ "Audio Access And Threading", "ara_design_overview.html#sec_AudioAccessAndThreading", null ]
    ] ],
    [ "Implementing ARA", "implementing_a_r_a.html", [
      [ "Preparing Your Implementation: Studying SDK Examples And Existing Products", "implementing_a_r_a.html#sec_UtilizingExamples", [
        [ "Mini Host", "implementing_a_r_a.html#autotoc_md7", null ],
        [ "Test Host and Test Plug-In", "implementing_a_r_a.html#autotoc_md8", null ],
        [ "JUCE_ARA", "implementing_a_r_a.html#autotoc_md9", null ],
        [ "Interaction With Existing Products", "implementing_a_r_a.html#autotoc_md10", null ]
      ] ],
      [ "Integrating The ARA SDK Into Your Products", "implementing_a_r_a.html#autotoc_md11", [
        [ "ARAInterface, Debug", "implementing_a_r_a.html#autotoc_md12", null ],
        [ "C++ Dispatcher, Utilities", "implementing_a_r_a.html#autotoc_md13", null ],
        [ "ARAPlug", "implementing_a_r_a.html#autotoc_md14", null ],
        [ "JUCE_ARA", "implementing_a_r_a.html#autotoc_md15", null ]
      ] ],
      [ "Mapping The Internal Model To ARA", "implementing_a_r_a.html#autotoc_md16", [
        [ "Dealing With Overlapping Playback Regions", "implementing_a_r_a.html#autotoc_md17", null ],
        [ "Modelling Audio Modifications In The Host", "implementing_a_r_a.html#autotoc_md18", null ],
        [ "Choosing An Appropriate Region Sequence Representation", "implementing_a_r_a.html#autotoc_md19", null ]
      ] ],
      [ "Configuring The Rendering", "implementing_a_r_a.html#autotoc_md20", [
        [ "Setting Up An ARA Playback Renderer", "implementing_a_r_a.html#autotoc_md21", null ],
        [ "Preview Rendering", "implementing_a_r_a.html#autotoc_md22", null ],
        [ "Conversion Between Audio Source and Song Sample Rate", "implementing_a_r_a.html#autotoc_md23", null ],
        [ "Playback Region Head And Tail Times", "implementing_a_r_a.html#autotoc_md24", null ],
        [ "Dealing With Denormals", "implementing_a_r_a.html#autotoc_md25", null ],
        [ "Caching Especially CPU-intense DSP", "implementing_a_r_a.html#autotoc_md26", null ]
      ] ],
      [ "Analyzing Audio Material", "implementing_a_r_a.html#autotoc_md27", [
        [ "What Can Be Analyzed?", "implementing_a_r_a.html#autotoc_md28", null ],
        [ "Manual Adjustments", "implementing_a_r_a.html#autotoc_md29", null ],
        [ "Triggering Explicit Analysis", "implementing_a_r_a.html#autotoc_md30", null ],
        [ "Algorithm Selection", "implementing_a_r_a.html#autotoc_md31", null ]
      ] ],
      [ "Utilizing Content Exchange", "implementing_a_r_a.html#autotoc_md32", [
        [ "Musical Timing Information", "implementing_a_r_a.html#sec_MusicalTimingInformation", null ],
        [ "Content Grade Examples", "implementing_a_r_a.html#autotoc_md33", null ],
        [ "Notes And How Playback Transformations Affect Content Data", "implementing_a_r_a.html#autotoc_md34", null ],
        [ "Chords, Key Signatures And Other Content Types", "implementing_a_r_a.html#autotoc_md35", null ]
      ] ],
      [ "Manipulating The Timing", "implementing_a_r_a.html#sec_ManipulatingTheTiming", null ],
      [ "Content Based Fades", "implementing_a_r_a.html#sec_ContentBasedFades", null ],
      [ "Managing ARA Archives", "implementing_a_r_a.html#sec_ManagingARAArchives", null ],
      [ "Partial Persistency", "implementing_a_r_a.html#sec_PartialPersistency", [
        [ "Copying ARA Data Between Documents", "implementing_a_r_a.html#autotoc_md36", null ],
        [ "Audio File Chunks", "implementing_a_r_a.html#sec_AudioFileChunks", null ]
      ] ],
      [ "Companion API Considerations", "implementing_a_r_a.html#autotoc_md37", [
        [ "Choosing Companion APIs", "implementing_a_r_a.html#sec_ChoosingCompanionAPIs", null ],
        [ "VST3: setActive() vs. setProcessing()", "implementing_a_r_a.html#autotoc_md38", null ],
        [ "Audio Unit: Optimizing Buffer Allocation", "implementing_a_r_a.html#autotoc_md39", null ]
      ] ],
      [ "User Interface Considerations", "implementing_a_r_a.html#autotoc_md40", [
        [ "View Embedding", "implementing_a_r_a.html#sec_ViewEmbedding", null ],
        [ "Reflecting Arrangement Selection In The Plug-In", "implementing_a_r_a.html#autotoc_md41", null ],
        [ "Windows High DPI View Scaling", "implementing_a_r_a.html#autotoc_md42", null ],
        [ "Key Event Handling", "implementing_a_r_a.html#autotoc_md43", null ]
      ] ],
      [ "Future ARA Development", "implementing_a_r_a.html#autotoc_md44", null ]
    ] ],
    [ "Use Cases and Testing", "test_cases.html", [
      [ "Synopsis", "test_cases.html#autotoc_md45", null ],
      [ "Render Timing", "test_cases.html#autotoc_md46", null ],
      [ "Musical Timing", "test_cases.html#autotoc_md47", null ],
      [ "Time Stretching", "test_cases.html#autotoc_md48", null ],
      [ "Signal Flow And Routing", "test_cases.html#autotoc_md49", null ],
      [ "Maintaining The ARA Model", "test_cases.html#autotoc_md50", null ],
      [ "Audio-MIDI Conversion", "test_cases.html#autotoc_md51", null ],
      [ "Key Signatures And Chords", "test_cases.html#autotoc_md52", null ],
      [ "Persistence", "test_cases.html#autotoc_md53", null ],
      [ "Versioning", "test_cases.html#autotoc_md54", null ],
      [ "UI-Related Topics", "test_cases.html#autotoc_md55", null ],
      [ "General", "test_cases.html#autotoc_md56", null ]
    ] ],
    [ "Module Documentation", "modules.html", "modules" ]
  ] ]
];

var NAVTREEINDEX =
[
"ara_design_overview.html",
"group___a_r_a___library___a_r_a_plug___plug_in_entry.html#a232f7e41a55da306cd0e3f5299a0d393",
"group___a_r_a___library___host___dispatch___plug-_in___interfaces.html#a920d4417abbc218175253107f549a37f",
"group___host___playback___controller___interface.html#struct_a_r_a_playback_controller_interface",
"group___plug-_in___factory.html#adba299d1cc79314d518fb546375ce5db"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';