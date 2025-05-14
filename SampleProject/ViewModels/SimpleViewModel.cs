using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace StartingApp.ViewModels
{

	public class SimpleViewModel : INotifyPropertyChanged
	{

		public event PropertyChangedEventHandler? PropertyChanged;
		private string? _Name;

		public string? Name
		{
			get
			{
				return _Name;
			}
			set
			{
				if (_Name != value)
				{
					_Name = value;

					RaisePropertyChanged();
					RaisePropertyChanged(nameof(Greeting));
				}
			}
		}

		public string Greeting
		{
			get
			{
				if (string.IsNullOrEmpty(Name))
				{
					return "Hello World from Avalonia.Samples";
				}
				else
				{
					return $"Hello {Name}";
				}
			}
		}

		private void RaisePropertyChanged([CallerMemberName] string? propertyName = null)
		{
			PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
		}
	}
}