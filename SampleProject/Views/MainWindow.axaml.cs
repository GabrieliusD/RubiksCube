using Avalonia.Controls;
using Avalonia.Interactivity;
using StartingApp.ViewModels;
using System;

namespace StartingApp.Views
{
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            InitializeComponent();
            Loaded += OnMainWindowLoaded;
        }

        private void OnMainWindowLoaded(object? sender, RoutedEventArgs e)
        {
            Loaded -= OnMainWindowLoaded;
            //Content = new WindowHost();
        }
    }
}