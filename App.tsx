import React from 'react';
import {Provider} from 'react-redux';
import {View, Text,ActivityIndicator} from 'react-native';
import  reduxStorage from './src/store/store';
import {BrowserViewControllerConnected} from './src/browser/BrowserViewController';
import {SafeAreaProvider} from 'react-native-safe-area-context';
import {NavigationContainer} from '@react-navigation/native';
import {createNativeStackNavigator} from '@react-navigation/native-stack';

import {Blog} from './src/browser/Blog';
import {About} from './src/browser/About';
import {Games} from './src/browser/Games';
import {Experiments} from './src/browser/Experiments';
import {Music} from './src/browser/Music';
import { PersistGate } from 'redux-persist/integration/react'
import root from './src/store/navigationsatateBysaga';
// import store from './src/store/store';
// import Blog from '../screens/Blog.tsx';
// import Experiments from '../screens/Experiments.tsx';
// import Games from '../screens/Games.tsx';
// import Music from '../screens/Music.tsx';
// import Constants from 'expo-constants';
// console.log("=====>expo unimodule",Constants.systemFonts);



interface Props {}

const {store,persistor} = reduxStorage();
// store.runSaga(root)
interface State {}
class AppContainer extends React.Component<Props, State> {

   constructor(props:any){
     super(props);
   
   }


  loaderComponet(){
        return (
          <View style={{justifyContent:"center",alignItems:"center",flex:1,backgroundColor:"#ffff"}}>
            <ActivityIndicator color="Blue" size="large" />
          </View>
        )
  }

  

  render() {
       
    
        // return ( <View style={{flex:1}}><Text>Hello</Text></View>)
    const {} = this.props;
    const Stack = createNativeStackNavigator();

    const NavigationContainer1 = () => {
      return (
        <Stack.Navigator
          initialRouteName="Home"
          screenOptions={{
            headerShown: false,
          }}>
          {/* <Stack.Screen name="Home" component={Home} /> */}
          {/* <Stack.Screen name="Home">
            {props => (
              <Home
                {...props}
                webView={barAwareWebView({
                  headerConfig: config.header,
                  scrollY: this.scrollY,
                  scrollEndDragVelocity: this.scrollEndDragVelocity,
                  navigation: props.navigation,
                })}
              />
            )}
          </Stack.Screen> */}
          <Stack.Screen
            name="Home"
            component={BrowserViewControllerConnected}
          />
          <Stack.Screen name="About" component={About} />
          <Stack.Screen name="Music" component={Music} />
          <Stack.Screen name="Experiments" component={Experiments} />
          <Stack.Screen name="Games" component={Games} />
          <Stack.Screen name="Blog" component={Blog} />
          {/* <Stack.Screen name="About">
            {props => (
              <About
                {...props}
                webView={barAwareWebView({
                  headerConfig: config.header,
                  scrollY: this.scrollY,
                  scrollEndDragVelocity: this.scrollEndDragVelocity,
                  navigation: props.navigation,
                })}
              />
            )}
          </Stack.Screen> */}
          {/* <Stack.Screen name="Blog">
            {props => (
              <Blog
                {...props}
                webView={barAwareWebView({
                  headerConfig: config.header,
                  scrollY: this.scrollY,
                  scrollEndDragVelocity: this.scrollEndDragVelocity,
                  navigation: props.navigation,
                })}
              />
            )}
          </Stack.Screen>
          <Stack.Screen name="Experiments">
            {props => (
              <Experiments
                {...props}
                webView={barAwareWebView({
                  headerConfig: config.header,
                  scrollY: this.scrollY,
                  scrollEndDragVelocity: this.scrollEndDragVelocity,
                  navigation: props.navigation,
                })}
              />
            )}
          </Stack.Screen>
          <Stack.Screen name="Games">
            {props => <Games {...props} webView={mainWebView} />}
          </Stack.Screen>
          <Stack.Screen name="Music">
            {props => (
              <Music
                {...props}
                webView={barAwareWebView({
                  headerConfig: config.header,
                  scrollY: this.scrollY,
                  scrollEndDragVelocity: this.scrollEndDragVelocity,
                  navigation: props.navigation,
                })}
              />
            )}
          </Stack.Screen> */}
          {/* <Stack.Screen name="Details" component={DetailsScreen} /> */}
        </Stack.Navigator>
      );
    };

  
    return (
      <SafeAreaProvider>
        <Provider store={store}>
        <PersistGate loading={this.loaderComponet()} persistor={persistor}>
          <NavigationContainer>
            {/* <BrowserViewControllerConnected /> */}
            <NavigationContainer1 />
          </NavigationContainer>
          </PersistGate>
        </Provider>
       
      </SafeAreaProvider>
    );
  }
}

export default AppContainer;
