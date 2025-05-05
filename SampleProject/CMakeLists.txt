cmake_minimum_required(VERSION 3.20)

project(SampleProject VERSION 1.0 LANGUAGES CSharp)

INCLUDE(CSharpUtilities)

# Set global .NET Framework version.
SET(CMAKE_DOTNET_SDK "Microsoft.NET.Sdk")
SET(CMAKE_DOTNET_TARGET_FRAMEWORK "net8.0")
#SET(CMAKE_DOTNET_TARGET_FRAMEWORK_VERSION "8.0")

# Set the C# language version for all projects.
SET(CMAKE_CSharp_FLAGS "/langversion:latest /unsafe")

add_executable(SampleProject 
	"Program.cs"

	"App.axaml"
	"App.axaml.cs"

	"ViewLocator.cs"

	"Helper/SampleWin.cs"
	"Helper/WinApi.cs"
	"Helper/WindowHost.cs"

	"ViewModels/MainWindowViewModel.cs"
	"ViewModels/ViewModelBase.cs"

	"Views/MainWindow.axaml"
	"Views/MainWindow.axaml.cs"
)

CSHARP_SET_XAML_CS_PROPERTIES(
	"App.axaml"
	"App.axaml.cs"
	"Views/MainWindow.axaml"
    "Views/MainWindow.axaml.cs"
)

#SET_PROPERTY(SOURCE "App.axaml" "Views/MainWindow.axaml" PROPERTY VS_XAML_TYPE "Avalonia XAML")
SET_SOURCE_FILES_PROPERTIES(App.axaml "Views/MainWindow.axaml" PROPERTIES VS_TOOL_OVERRIDE "AvaloniaXAML")

# Set CLR assembly properties.
SET_TARGET_PROPERTIES(SampleProject PROPERTIES
	WIN32_EXECUTABLE TRUE
    VS_DOTNET_REFERENCES "System;System.Core;System.Data;System.Data.DataSetExtensions;System.Drawing;System.Net.Http;System.Xaml;System.Xml.Linq;Microsoft.CSharp;PresentationCore;PresentationFramework;WindowsBase"
    VS_GLOBAL_ROOTNAMESPACE SampleProject
	VS_PACKAGE_REFERENCES "Avalonia_11.2.1;Avalonia.Desktop_11.2.1;Avalonia.Themes.Fluent_11.2.1;Avalonia.Fonts.Inter_11.2.1;Avalonia.Diagnostics_11.2.1;CommunityToolkit.Mvvm_8.2.1"
)